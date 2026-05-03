/*
 * Arena base-write coverage harness over the test262 corpus.
 *
 * For every test file under TEST_ROOT (default test262/test/built-ins):
 *   - eval the test body in arena mode
 *   - reset the request arena
 *   - record how many base bytes were dirtied
 *
 * We do NOT care whether the test passes/fails the ECMAScript spec.
 * The signal we want is: did request-time JS code mutate snapshot
 * memory? Any non-zero count is a bug to investigate.
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
#define ARENA_REQ     (16 * 1024 * 1024)

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

/* Returns true if the test frontmatter declares any flag/feature we
   don't support yet. Conservative — when in doubt, skip. */
static int test_is_skippable(const char *src, size_t len)
{
    /* test262 frontmatter is `/*---` ... `---*\/` near the top. */
    const char *fm = strstr(src, "/*---");
    if (!fm) return 1; /* no frontmatter -> not test262, skip */
    const char *fmend = strstr(fm, "---*/");
    if (!fmend) return 1;
    size_t fmlen = fmend - fm;
    /* Skip module/async/raw forms; we don't drive those here. */
    if (memmem(fm, fmlen, "[module]", 8)) return 1;
    if (memmem(fm, fmlen, "[async]", 7)) return 1;
    if (memmem(fm, fmlen, "[raw]", 5)) return 1;
    /* Skip negative parser tests — they exercise SyntaxError paths
       that are noisy for our purposes. */
    if (memmem(fm, fmlen, "phase: parse", 12)) return 1;
    if (memmem(fm, fmlen, "phase: early", 12)) return 1;
    /* Skip features that need preloaded includes we don't ship. */
    if (memmem(fm, fmlen, "includes:", 9)) {
        /* Allow tests that only need assert.js/sta.js — those are preloaded. */
        const char *inc = (const char *)memmem(fm, fmlen, "includes:", 9);
        const char *incend = strchr(inc, '\n');
        if (incend && incend > inc) {
            size_t inclen = incend - inc;
            /* If it mentions anything besides assert.js/sta.js, skip. */
            for (size_t i = 0; i < inclen - 4; i++) {
                if (inc[i] == '.' && inc[i+1] == 'j' && inc[i+2] == 's') {
                    /* find start of name */
                    size_t s = i;
                    while (s > 0 && (inc[s-1] == '-' || inc[s-1] == '_'
                            || inc[s-1] == '.' || inc[s-1] == '/'
                            || (inc[s-1] >= 'a' && inc[s-1] <= 'z')
                            || (inc[s-1] >= 'A' && inc[s-1] <= 'Z')
                            || (inc[s-1] >= '0' && inc[s-1] <= '9')))
                        s--;
                    size_t namelen = i + 3 - s;
                    if (!(namelen == 9 && !memcmp(inc + s, "assert.js", 9))
                     && !(namelen == 6 && !memcmp(inc + s, "sta.js", 6)))
                        return 1;
                }
            }
        }
    }
    (void)len;
    return 0;
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

typedef struct {
    int total;
    int skipped;
    int evaluated;
    int eval_exception;
    int base_clean;
    int base_dirtied;
    size_t total_dirty_bytes;
    int max_files;
    JSRuntime *rt;
    JSContext *ctx;
} Walker;

static void walk(Walker *w, const char *root, const char *prefix);

static void run_one(Walker *w, const char *path, const char *relpath)
{
    if (w->max_files > 0 && w->evaluated >= w->max_files) return;
    size_t len;
    char *src = slurp(path, &len);
    if (!src) return;
    w->total++;
    if (test_is_skippable(src, len)) {
        w->skipped++;
        free(src);
        return;
    }
    js_arena_thermometer_reset();
    JSValue v = JS_Eval(w->ctx, src, len, relpath, JS_EVAL_TYPE_GLOBAL);
    size_t pages = js_arena_thermometer_pages();
    size_t bytes = js_arena_thermometer_changed_bytes();
    int exc = JS_IsException(v);
    if (exc) {
        JSValue e = JS_GetException(w->ctx);
        JS_FreeValue(w->ctx, e);
        w->eval_exception++;
    }
    JS_FreeValue(w->ctx, v);
    JS_ResetRequestArena(w->rt);
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

    JSRuntime *rt = JS_NewRuntimeArena(ARENA_BASE, ARENA_REQ);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 1; }

    /* Preload the test262 harness into base so per-test runs only
       account for the test body's allocations. */
    if (eval_file_into_base(ctx, HARNESS_DIR "/sta.js")) return 1;
    if (eval_file_into_base(ctx, HARNESS_DIR "/assert.js")) return 1;
    /* compareArray + deepEqual are common — preload too. */
    if (eval_file_into_base(ctx, HARNESS_DIR "/compareArray.js")) return 1;

    JS_FreezeRuntime(rt);
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 1;
    }

    fprintf(stderr, "scanning %s%s...\n", root,
            max_files ? " (limited)" : "");

    Walker w = {0};
    w.rt = rt; w.ctx = ctx; w.max_files = max_files;
    walk(&w, root, "");

    qsort(findings, n_findings, sizeof(Finding), finding_cmp);

    printf("\n=== arena-test262 summary ===\n");
    printf("  files scanned:       %d\n", w.total);
    printf("  skipped (tagged):    %d\n", w.skipped);
    printf("  evaluated:           %d\n", w.evaluated);
    printf("    eval threw:        %d\n", w.eval_exception);
    printf("    base clean:        %d\n", w.base_clean);
    printf("    base dirtied:      %d  (%zu bytes total)\n",
           w.base_dirtied, w.total_dirty_bytes);

    int show = n_findings < 25 ? n_findings : 25;
    if (show > 0) {
        printf("\n  worst %d offenders:\n", show);
        for (int i = 0; i < show; i++) {
            printf("    %6zu B  %3zu pg  %s\n",
                   findings[i].bytes, findings[i].pages, findings[i].path);
        }
    }

    js_arena_thermometer_disable();
    js_dual_arena_free(JS_GetDualArena(rt));
    return w.base_dirtied > 0 ? 1 : 0;
}
