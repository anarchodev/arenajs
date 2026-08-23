/*
 * Arena base-write coverage harness over the test262 corpus.
 *
 * For every test file under TEST_ROOT (default test262/test/built-ins):
 *   - eval the test body in arena mode
 *   - reset the request arena
 *   - record how many base bytes were dirtied
 *
 * Two independent signals, both wanted:
 *
 *   1. base writes  - did request-time JS mutate snapshot memory?
 *      Any non-zero count is a bug to investigate.
 *
 *   2. spec outcome - did the test actually PASS, under an arena
 *      runtime? This is not redundant with run-test262: that harness
 *      builds vanilla runtimes (JS_NewRuntime, not JS_NewRuntimeArena),
 *      so the shadow mechanism never engages and no arena-specific
 *      correctness bug can ever reach it. A spec violation that only
 *      appears when `this` arrives as a shadow -- the Error.prototype
 *      .stack home-object guard, say, which test262 covers with 35
 *      files -- passes run-test262 and is invisible here too unless we
 *      check outcomes. Hence this.
 *
 * The test262 harness (assert.js, sta.js) is preloaded at snapshot
 * build time so its allocations live in base, not in the per-test
 * request arena, and so harness execution is not attributed to any
 * single test.
 *
 * Skipped:
 *   - tests tagged [module] or [async] (different driver needed)
 *   - tests that need extra includes we haven't preloaded
 *   - non-.js files
 *
 * Build:
 *   cc -I. -O2 arena-test262.c build/libqjs.a -lm -lpthread \
 *       -o build/arena-test262
 *
 * Usage:
 *   build/arena-test262 [root_dir] [max_files]
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "quickjs.h"
#include "qjs-arena.h"

#define DEFAULT_ROOT  "test262/test/built-ins"
#define HARNESS_DIR   "test262/harness"
#define ARENA_BASE    (64 * 1024 * 1024)
/* 16 MB was not enough: the RegExp CharacterClassEscapes tests build very
   large match sets and hit "InternalError: out of memory in regexp
   execution", which is a harness sizing artefact rather than an engine
   result -- the same files pass on a vanilla runtime. Sized so the corpus
   runs without the allocator becoming the thing under test. */
#define ARENA_REQ     (128 * 1024 * 1024)

static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    char *buf = malloc(st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = read(fd, buf, st.st_size);
    close(fd);
    if (n != st.st_size) { free(buf); return NULL; }
    buf[n] = 0;
    if (out_len) *out_len = (size_t)n;
    return buf;
}

/* Harness files preloaded into base. This list is the single source of
   truth: test_is_skippable() checks a test's `includes:` against it, so
   adding a file here immediately widens coverage and removing one
   immediately narrows it, with no second list to forget.

   Order matters -- sta.js defines Test262Error, assert.js needs it, and
   several of the rest need assert. */
static const char *PRELOAD[] = {
    "sta.js", "assert.js", "compareArray.js", "propertyHelper.js",
    "isConstructor.js", "nativeErrors.js", "proxyTrapsHelper.js",
    "testTypedArray.js", "regExpUtils.js", "wellKnownIntrinsicObjects.js",
    "deepEqual.js", "fnGlobalObject.js", "decimalToHexString.js",
    "assertRelativeDateMs.js",
    /* detachArrayBuffer.js is deliberately absent: it is a $262 shim, so
       preloading it would only move the failure from load to call time. */
};
#define PRELOAD_COUNT ((int)(sizeof PRELOAD / sizeof PRELOAD[0]))

static int is_preloaded(const char *name, size_t len)
{
    for (int i = 0; i < PRELOAD_COUNT; i++)
        if (strlen(PRELOAD[i]) == len && !memcmp(PRELOAD[i], name, len))
            return 1;
    return 0;
}

/* test262 flags are a YAML list and appear in several shapes:
       flags: [module]
       flags: [generated, module]
       flags:
         - module
   Matching the literal "[module]" -- as this harness used to -- catches
   only the first, so module and async tests leaked through and failed as
   SyntaxError / ReferenceError rather than being skipped. Match the flag
   as a whole word inside the frontmatter's flags: section instead. */
static int has_flag(const char *fm, size_t fmlen, const char *flag)
{
    const char *f = (const char *)memmem(fm, fmlen, "flags:", 6);
    if (!f) return 0;
    size_t rest = (size_t)(fm + fmlen - f);
    size_t flen = strlen(flag);
    for (const char *q = f; q + flen <= f + rest; q++) {
        if (memcmp(q, flag, flen)) continue;
        char before = (q == f) ? ' ' : q[-1];
        char after  = q[flen];
        if (!isalnum((unsigned char)before) && before != '_' &&
            !isalnum((unsigned char)after)  && after  != '_')
            return 1;
    }
    return 0;
}

/* Features this build does not provide at all. A test naming one of these
   in its `features:` is out of scope, not a failure -- run-test262 skips
   the same set via the [features] section of test262.conf. Verified against
   the build: typeof Intl === typeof Temporal === typeof ShadowRealm ===
   "undefined". Together with $DONE these account for 8912 of the 10730
   failures the first baseline run produced, none of them arena-related. */
static const char *MISSING_FEATURES[] = {
    "Temporal", "Intl", "ShadowRealm", "decorators",
};
#define MISSING_FEATURE_COUNT ((int)(sizeof MISSING_FEATURES / sizeof MISSING_FEATURES[0]))

/* Returns true if the test frontmatter declares any flag/feature we
   don't support yet. Conservative — when in doubt, skip. */
static int test_is_skippable(const char *src, size_t len)
{
    /* test262 frontmatter is a block comment opening with three dashes
       and closing with three dashes, near the top of the file. */
    const char *fm = strstr(src, "/*---");
    if (!fm) return 1; /* no frontmatter -> not test262, skip */
    const char *fmend = strstr(fm, "---*/");
    if (!fmend) return 1;
    size_t fmlen = fmend - fm;
    /* Async tests are driven by $DONE, which needs a completion callback we
       do not install. The frontmatter check below catches `flags: [async]`
       but not the multi-line YAML form, so key off the driver symbol -- it
       is present in every async test by construction. */
    if (memmem(src, len, "$DONE", 5)) return 1;
    /* Unimplemented features: out of scope, not failures. */
    {
        const char *fe = (const char *)memmem(fm, fmlen, "features:", 9);
        if (fe) {
            size_t rest = (size_t)(fmend - fe);
            for (int i = 0; i < MISSING_FEATURE_COUNT; i++)
                if (memmem(fe, rest, MISSING_FEATURES[i],
                           strlen(MISSING_FEATURES[i])))
                    return 1;
        }
    }
    /* The walker provides no $262 host object (no cross-realm, no
       detachArrayBuffer, no agent). Tests reaching for it are out of
       scope here, not failures -- run-test262 covers them. */
    if (memmem(src, len, "$262", 4)) return 1;
    /* Skip module/async/raw forms; we don't drive those here. */
    if (has_flag(fm, fmlen, "module")) return 1;
    if (has_flag(fm, fmlen, "async")) return 1;
    if (has_flag(fm, fmlen, "raw")) return 1;
    /* Skip negative parser tests — they exercise SyntaxError paths
       that are noisy for our purposes. */
    if (memmem(fm, fmlen, "phase: parse", 12)) return 1;
    if (memmem(fm, fmlen, "phase: early", 12)) return 1;
    /* Skip tests needing a harness file we have not preloaded. test262
       writes `includes:` two ways --
           includes: [foo.js, bar.js]
           includes:
             - foo.js
       -- and the old scan only handled the first, so multi-line tests ran
       without their helpers and failed with ReferenceError. Scan the whole
       frontmatter for .js names instead and check each against PRELOAD. */
    {
        const char *inc = (const char *)memmem(fm, fmlen, "includes:", 9);
        if (inc) {
            for (const char *q = inc; q + 3 <= fmend; q++) {
                if (q[0] != '.' || q[1] != 'j' || q[2] != 's') continue;
                const char *e = q + 3;
                const char *b = q;
                while (b > inc && (isalnum((unsigned char)b[-1]) ||
                       b[-1] == '-' || b[-1] == '_' || b[-1] == '.'))
                    b--;
                if (!is_preloaded(b, (size_t)(e - b))) return 1;
            }
        }
    }
    (void)len;
    return 0;
}

/* test262 `flags: [onlyStrict]` means the file is only meaningful under
   strict mode. Evaluating it sloppy makes `this` the global object where
   the test expects undefined, and silences the TypeErrors that strictness
   is what produces -- so those tests fail for a reason that has nothing to
   do with the engine. Detect the flag; run_one prepends the directive.

   flags: [noStrict] and unflagged files are fine sloppy, which is what we
   already do. Unflagged files are meant to run BOTH ways; we run one, so
   this is a coverage limit rather than a wrong answer. */
static int test_is_only_strict(const char *src, size_t len)
{
    const char *fm = strstr(src, "/*---");
    if (!fm) return 0;
    const char *fmend = strstr(fm, "---*/");
    if (!fmend) return 0;
    return has_flag(fm, (size_t)(fmend - fm), "onlyStrict");
}

/* test262 `negative:` frontmatter names the error the test must throw:
       negative:
         phase: runtime
         type: TypeError
   Returns 1 and fills `type` when present. phase: parse/early are skipped
   upstream of here, so anything we see is a runtime expectation. */
static int parse_negative_type(const char *src, size_t len,
                               char *type, size_t typesz)
{
    const char *fm = strstr(src, "/*---");
    if (!fm) return 0;
    const char *fmend = strstr(fm, "---*/");
    if (!fmend) return 0;
    size_t fmlen = fmend - fm;
    if (!memmem(fm, fmlen, "negative:", 9)) return 0;
    const char *t = (const char *)memmem(fm, fmlen, "type:", 5);
    if (!t) return 0;
    t += 5;
    while (t < fmend && (*t == ' ' || *t == '\t')) t++;
    size_t i = 0;
    while (t < fmend && i + 1 < typesz &&
           ((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z') ||
            (*t >= '0' && *t <= '9') || *t == '_'))
        type[i++] = *t++;
    type[i] = 0;
    (void)len;
    return i > 0;
}

typedef struct {
    char *path;          /* relative to root */
    size_t bytes;        /* base bytes dirtied */
    size_t pages;
} Finding;

static int finding_cmp(const void *a, const void *b)
{
    size_t sa = ((const Finding *)a)->bytes;
    size_t sb = ((const Finding *)b)->bytes;
    if (sa < sb) return 1;
    if (sa > sb) return -1;
    return 0;
}

static Finding *findings = NULL;
static size_t  n_findings = 0;
static size_t  cap_findings = 0;

static void record_finding(const char *path, size_t bytes, size_t pages)
{
    if (n_findings == cap_findings) {
        cap_findings = cap_findings ? cap_findings * 2 : 64;
        findings = realloc(findings, cap_findings * sizeof(Finding));
    }
    findings[n_findings].path  = strdup(path);
    findings[n_findings].bytes = bytes;
    findings[n_findings].pages = pages;
    n_findings++;
}

/* ---- expected-failure baseline ----
 *
 * Same contract as test262_errors.txt, so the two harnesses behave alike:
 * a recorded failure is expected, an unrecorded one is NEW, a recorded one
 * whose message changed is CHANGED, and a recorded one that now passes is
 * FIXED. All three are errors -- FIXED included, because a stale baseline
 * quietly stops gating.
 *
 * Keyed on the path as invoked (test262/test/...), so entries are stable
 * across runs the way the Makefile drives it.
 *
 * Regenerate with ARENA_BASELINE_UPDATE=1.
 */
#define BASELINE_FILE "arena_test262_errors.txt"

typedef struct { char *path; char *why; int seen; } Expected;
static Expected *expected;
static int expected_count, expected_cap;
static FILE *baseline_out;   /* non-NULL only under ARENA_BASELINE_UPDATE */
static int skip_intl402;
static int vanilla_rt;

static void baseline_load(void)
{
    FILE *f = fopen(BASELINE_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char *sep = strstr(line, ": ");
        if (!sep) continue;
        *sep = 0;
        if (expected_count == expected_cap) {
            expected_cap = expected_cap ? expected_cap * 2 : 256;
            expected = realloc(expected, (size_t)expected_cap * sizeof *expected);
        }
        expected[expected_count].path = strdup(line);
        expected[expected_count].why  = strdup(sep + 2);
        expected[expected_count].seen = 0;
        expected_count++;
    }
    fclose(f);
}

static Expected *baseline_find(const char *path)
{
    for (int i = 0; i < expected_count; i++)
        if (!strcmp(expected[i].path, path))
            return &expected[i];
    return NULL;
}

/* Spec-outcome failures, kept separate from base-write findings: a test
   can fail the spec while leaving base pristine, and vice versa. */
typedef struct { char *path; char *why; } Failure;
static Failure failures[512];
static int failure_count;

static void record_failure(const char *relpath, const char *why)
{
    if (failure_count >= (int)(sizeof failures / sizeof failures[0])) return;
    failures[failure_count].path = strdup(relpath);
    failures[failure_count].why  = strdup(why);
    failure_count++;
}

typedef struct {
    int total;
    int skipped;
    int evaluated;
    int eval_exception;
    int base_clean;
    int base_dirtied;
    int spec_pass;
    int spec_fail;      /* expected, per the baseline */
    int spec_new;       /* not in the baseline -- a regression */
    int spec_changed;   /* in the baseline, different message */
    int spec_fixed;     /* in the baseline but now passing */
    int oom;            /* request arena exhausted -- capacity, not a verdict */
    size_t total_dirty_bytes;
    int max_files;
    JSRuntime *rt;
    JSContext *ctx;
} Walker;

static int eval_file_into_base(JSContext *ctx, const char *path);
static void walk(Walker *w, const char *root, const char *prefix);

static void run_one(Walker *w, const char *path, const char *relpath)
{
    if (w->max_files > 0 && w->evaluated >= w->max_files) return;
    size_t len;
    char *src = slurp(path, &len);
    if (!src) return;
    w->total++;
    if (skip_intl402 && strstr(path, "intl402")) {
        w->skipped++;
        free(src);
        return;
    }
    if (test_is_skippable(src, len)) {
        w->skipped++;
        free(src);
        return;
    }
    if (getenv("ARENA_VERBOSE")) {
        fprintf(stderr, "  [%d] %s\n", w->evaluated + 1, relpath);
        fflush(stderr);
    }
    char neg_type[64];
    int is_negative = parse_negative_type(src, len, neg_type, sizeof neg_type);

    /* A directive prologue must be the first *statement*; leading comments
       do not displace it, so prefixing the file is enough. */
    char *strict_src = NULL;
    const char *eval_src = src;
    size_t eval_len = len;
    if (test_is_only_strict(src, len)) {
        static const char PRO[] = "\"use strict\";\n";
        strict_src = malloc(sizeof PRO - 1 + len + 1);
        if (strict_src) {
            memcpy(strict_src, PRO, sizeof PRO - 1);
            memcpy(strict_src + sizeof PRO - 1, src, len + 1);
            eval_src = strict_src;
            eval_len = sizeof PRO - 1 + len;
        }
    }

    if (!vanilla_rt) js_arena_thermometer_reset();
    JSValue v = JS_Eval(w->ctx, eval_src, eval_len, relpath, JS_EVAL_TYPE_GLOBAL);
    size_t pages = vanilla_rt ? 0 : js_arena_thermometer_pages();
    size_t bytes = vanilla_rt ? 0 : js_arena_thermometer_changed_bytes();
    int exc = JS_IsException(v);
    char why[320]; why[0] = 0;
    if (exc) {
        JSValue e = JS_GetException(w->ctx);
        const char *es = JS_ToCString(w->ctx, e);
        w->eval_exception++;
        if (!is_negative) {
            /* An uncaught throw is a failure: assert.js signals assertion
               failures by throwing Test262Error. */
            snprintf(why, sizeof why, "threw: %s", es ? es : "?");
        } else if (es && strncmp(es, neg_type, strlen(neg_type)) != 0) {
            snprintf(why, sizeof why, "expected %s, got: %s", neg_type, es);
        }
        if (es) JS_FreeCString(w->ctx, es);
        JS_FreeValue(w->ctx, e);
    } else if (is_negative) {
        snprintf(why, sizeof why, "expected %s, no exception", neg_type);
    }
    /* An exhausted request arena is a capacity result, not a wrong answer.
       It is the documented signal in BUMP mode, where the ceiling is
       CUMULATIVE allocation rather than peak-live -- an allocation-heavy
       test (annexB's RegExp BMP sweeps eval ~65k patterns) will hit it by
       construction. Report it separately so a mode's capacity envelope
       cannot masquerade as a spec divergence, and do not let it drift the
       shared baseline. */
    if (!vanilla_rt && js_dual_arena_oom_hit(JS_GetDualArena(w->rt))) {
        w->oom++;
        why[0] = 0;
    }

    {
        Expected *e = baseline_find(path);
        if (e) e->seen = 1;
        if (why[0]) {
            if (!e) {
                w->spec_new++;
                record_failure(path, why);
            } else if (strcmp(e->why, why)) {
                w->spec_changed++;
                char buf[640];
                snprintf(buf, sizeof buf, "was: %s\n      now: %s", e->why, why);
                record_failure(path, buf);
            } else {
                w->spec_fail++;
            }
            if (baseline_out) fprintf(baseline_out, "%s: %s\n", path, why);
        } else {
            if (e) { w->spec_fixed++; record_failure(path, "now PASSES -- drop from baseline"); }
            w->spec_pass++;
        }
    }
    JS_FreeValue(w->ctx, v);
    free(strict_src);
    if (!vanilla_rt) {
        JS_ResetRequestArena(w->rt);
    } else {
        /* No request arena to rewind, so isolation has to come from a fresh
           context: without it each test's globals leak into the next and the
           run collapses into `redeclaration of 'C'` / `Array is not defined`.
           The arena walker gets this for free from the reset. */
        JS_FreeContext(w->ctx);
        w->ctx = JS_NewContext(w->rt);
        for (int i = 0; i < PRELOAD_COUNT; i++) {
            char hp[512];
            snprintf(hp, sizeof hp, "%s/%s", HARNESS_DIR, PRELOAD[i]);
            if (eval_file_into_base(w->ctx, hp)) {
                fprintf(stderr, "vanilla: harness reload failed\n"); exit(1);
            }
        }
    }
    w->evaluated++;
    if (bytes == 0 && pages == 0) {
        w->base_clean++;
    } else {
        w->base_dirtied++;
        w->total_dirty_bytes += bytes;
        record_finding(relpath, bytes, pages);
    }
    if ((w->evaluated & 1023) == 0) {
        fprintf(stderr, "  ... %d evaluated, %d clean, %d dirtied\n",
                w->evaluated, w->base_clean, w->base_dirtied);
    }
    free(src);
}

static void walk(Walker *w, const char *root, const char *prefix)
{
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[4096], rel[4096];
        snprintf(full, sizeof full, "%s/%s", root, de->d_name);
        if (prefix && *prefix)
            snprintf(rel, sizeof rel, "%s/%s", prefix, de->d_name);
        else
            snprintf(rel, sizeof rel, "%s", de->d_name);
        struct stat st;
        if (stat(full, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(w, full, rel);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(de->d_name);
            if (nlen > 3 && !strcmp(de->d_name + nlen - 3, ".js")) {
                /* test262 ships some `*_FIXTURE.js` files that are
                   loaded by other tests, not standalone — skip them. */
                if (strstr(de->d_name, "_FIXTURE")) continue;
                run_one(w, full, rel);
            }
        }
        if (w->max_files > 0 && w->evaluated >= w->max_files) break;
    }
    closedir(d);
}

static int eval_file_into_base(JSContext *ctx, const char *path)
{
    size_t len;
    char *src = slurp(path, &len);
    if (!src) {
        fprintf(stderr, "preload: missing %s\n", path);
        return -1;
    }
    JSValue v = JS_Eval(ctx, src, len, path, JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "preload: %s failed: %s\n", path, s ? s : "(no msg)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : DEFAULT_ROOT;
    int max_files = argc > 2 ? atoi(argv[2]) : 0;

    /* ARENA_RUNTIME=vanilla builds a plain runtime instead, so the SAME
       harness -- same skip rules, same preloads, same strict handling, same
       pass/fail judging -- can walk the corpus with the arena out of the
       picture. Diffing the two failure sets attributes a divergence to the
       arena unambiguously.

       Doing this here rather than comparing against run-test262 is
       deliberate: that harness has its own skip set (test262.conf features),
       its own strict-mode policy, and -f/-a flag semantics that make a
       per-file comparison quietly wrong -- a skipped test prints nothing and
       reads as a pass. Same-harness is the only comparison that isolates the
       variable we care about. */
    vanilla_rt = getenv("ARENA_RUNTIME") &&
                 !strcmp(getenv("ARENA_RUNTIME"), "vanilla");
    JSRuntime *rt = vanilla_rt ? JS_NewRuntime()
                               : JS_NewRuntimeArena(ARENA_BASE, ARENA_REQ);
    if (!rt) { fprintf(stderr, "runtime creation failed\n"); return 1; }
    if (vanilla_rt) fprintf(stderr, "runtime: VANILLA (no arena)\n");
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 1; }

    /* Preload the test262 harness into base so per-test runs only
       account for the test body's allocations. */

    /* intl402 is the Intl suite by construction; its tests do not declare
       `features: [Intl...]` because the directory IS the feature. With no
       Intl in this build they die on the first line, which gives neither a
       spec result nor any base-write coverage worth having -- 318 of 341
       were "Intl is not defined". Probe rather than assume, so this
       disables itself if Intl is ever implemented. */
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue i = JS_GetPropertyStr(ctx, g, "Intl");
        skip_intl402 = JS_IsUndefined(i);
        JS_FreeValue(ctx, i);
        JS_FreeValue(ctx, g);
        if (skip_intl402)
            fprintf(stderr, "no Intl in this build: skipping intl402\n");
    }

    baseline_load();
    if (getenv("ARENA_BASELINE_UPDATE")) {
        baseline_out = fopen(BASELINE_FILE ".new", "w");
        if (!baseline_out) { perror(BASELINE_FILE ".new"); return 1; }
        fprintf(stderr, "baseline update -> %s\n", BASELINE_FILE ".new");
    }

    for (int i = 0; i < PRELOAD_COUNT; i++) {
        char hp[512];
        snprintf(hp, sizeof hp, "%s/%s", HARNESS_DIR, PRELOAD[i]);
        if (eval_file_into_base(ctx, hp)) return 1;
    }

    if (!vanilla_rt) JS_FreezeRuntime(rt);
    if (!vanilla_rt && getenv("ARENA_HARDEN")) {
        /* MMU-enforced mode: base goes PROT_READ and any base write
           kills the walker with an [arena-harden] diagnostic. Replaces
           the thermometer (mutually exclusive); the per-test dirty
           counters read 0 and SURVIVAL is the assertion. */
        if (js_dual_arena_harden(JS_GetDualArena(rt)) != 0) {
            fprintf(stderr, "harden failed\n"); return 1;
        }
        fprintf(stderr, "base HARDENED (PROT_READ): survival == zero base writes\n");
    } else if (!vanilla_rt && js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 1;
    }

    /* Request-allocator mode. GC is the default; ARENA_REQ_MODE=bump walks
       the whole corpus on the bump cursor instead, where js_free is a no-op,
       the cycle collector is off and the ceiling is cumulative rather than
       peak-live. Those are materially different paths -- the 9de2921
       refcount relocation touched both -- so the corpus is worth running
       twice.

       Deliberately ONE baseline for both modes: the allocator regime must
       not change observable semantics, so a test that fails in one mode and
       not the other is a finding, and sharing the file is what surfaces it.

       set_request_mode takes effect at the next reset, so reset once here
       before the first test rather than letting test #1 run under GC.
       Must come AFTER JS_FreezeRuntime: rt->req is created at freeze, so
       resetting before it exists segfaults immediately. */
    int want_bump = getenv("ARENA_REQ_MODE") &&
                    !strcmp(getenv("ARENA_REQ_MODE"), "bump");
    if (want_bump) {
        js_dual_arena_set_request_mode(JS_GetDualArena(rt),
                                       JS_ARENA_REQ_MODE_BUMP);
        JS_ResetRequestArena(rt);
    }
    fprintf(stderr, "request mode: %s\n", want_bump ? "BUMP" : "GC");

    fprintf(stderr, "scanning %s%s...\n", root,
            max_files ? " (limited)" : "");

    Walker w = {0};
    w.rt = rt; w.ctx = ctx; w.max_files = max_files;
    /* A regular file as `root` is read as a newline-separated list of test
       paths. Lets a run target exactly the set under investigation -- e.g.
       replaying the arena baseline on a vanilla runtime -- instead of
       sweeping the corpus to reach a few hundred files. */
    {
        struct stat st;
        if (!stat(root, &st) && S_ISREG(st.st_mode)) {
            FILE *lf = fopen(root, "r");
            if (!lf) { perror(root); return 1; }
            char line[1024];
            while (fgets(line, sizeof line, lf)) {
                char *nl = strchr(line, '\n'); if (nl) *nl = 0;
                char *colon = strstr(line, ": ");   /* tolerate "path: why" */
                if (colon) *colon = 0;
                if (line[0]) run_one(&w, line, line);
            }
            fclose(lf);
        } else {
    walk(&w, root, "");
        }
    }

    qsort(findings, n_findings, sizeof(Finding), finding_cmp);

    printf("\n=== arena-test262 summary ===\n");
    printf("  files scanned:       %d\n", w.total);
    printf("  skipped (tagged):    %d\n", w.skipped);
    printf("  evaluated:           %d\n", w.evaluated);
    printf("    eval threw:        %d\n", w.eval_exception);
    printf("    base clean:        %d\n", w.base_clean);
    printf("    base dirtied:      %d  (%zu bytes total)\n",
           w.base_dirtied, w.total_dirty_bytes);

    printf("    spec pass:         %d\n", w.spec_pass);
    printf("    spec fail (known): %d\n", w.spec_fail);
    printf("    spec NEW:          %d\n", w.spec_new);
    printf("    spec CHANGED:      %d\n", w.spec_changed);
    printf("    spec FIXED:        %d\n", w.spec_fixed);
    printf("    arena OOM:         %d  (capacity, not a verdict)\n", w.oom);

    int show = n_findings < 25 ? n_findings : 25;
    if (show > 0) {
        printf("\n  worst %d offenders:\n", show);
        for (int i = 0; i < show; i++) {
            printf("    %6zu B  %3zu pg  %s\n",
                   findings[i].bytes, findings[i].pages, findings[i].path);
        }
    }

    int fshow = failure_count < 40 ? failure_count : 40;
    if (fshow > 0) {
        printf("\n  spec drift (first %d of %d) -- NEW / CHANGED / FIXED:\n", fshow, failure_count);
        for (int i = 0; i < fshow; i++)
            printf("    %s\n      %s\n", failures[i].path, failures[i].why);
    }

    if (baseline_out) {
        fclose(baseline_out);
        fprintf(stderr, "wrote %s.new -- review, then mv over %s\n",
                BASELINE_FILE, BASELINE_FILE);
    }

    if (!vanilla_rt) {
        js_arena_thermometer_disable();
        js_dual_arena_free(JS_GetDualArena(rt));
    }
    /* Known failures do not gate; drift from the baseline does, in either
       direction. base_dirtied always gates -- it has no baseline and never
       should. */
    return (w.base_dirtied > 0 || w.spec_new > 0 ||
            w.spec_changed > 0 || w.spec_fixed > 0) ? 1 : 0;
}
