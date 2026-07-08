/*
 * arena_multi_reactor — exercise the multi-instance contract
 * (qjs-arena-reactor.h): two reactors on one thread, runs nested
 * across them, with per-instance isolation of determinism pins, trace
 * state, entry-module resolution, request-allocator regime, and OOM
 * records.
 *
 * Shape mirrors rove's rewind test runner: a "harness" reactor (A)
 * whose kv responder synchronously runs modules on a "sim" reactor (B)
 * mid-request — the exact configuration the de-singletoned reactor
 * exists to support. The host callbacks are process-global by
 * contract, so this driver multiplexes with a current-instance marker
 * it flips around each run (runs are synchronous — the host always
 * knows whose run is active).
 *
 * Asserts:
 *   - pins don't bleed: A sees its Date.now/PRNG pins after B's nested
 *     run re-applied B's;
 *   - trace isolation: B's nested runs don't clear A's NAME intern
 *     table (no duplicate NAME for a function A calls before and after
 *     the nest), and a stop armed on B never silences A's hooks;
 *   - entry-ns per instance: __arena_entry_ns() resolves each module's
 *     own namespace, stable across a nested foreign run;
 *   - request mode + OOM records are per instance: B in bump mode
 *     exhausts its small arena (rc ARENA_RC_OOM, oom_hit=1) while A in
 *     GC mode runs the same churn clean (rc 0, oom_hit=0).
 *
 * Build:
 *   cmake -B build -DQJS_BUILD_EXAMPLES=ON
 *   cmake --build build --target arena_multi_reactor
 *   ./build/arena_multi_reactor
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qjs-arena-reactor.h"
#include "qjs-arena-trace.h"
#include "qjs-arena-replay-bindings.h"

static ArenaReactor *g_A, *g_B;

/* Which instance's run is active — flipped by the driver around each
   run so the global callbacks can attribute events. */
static char g_cur = '-';

/* ── captured kv.set values, keyed "<instance>:<key>" ─────────────────── */

#define CAP_MAX 32
static struct { char key[64]; char val[128]; } g_cap[CAP_MAX];
static int g_ncap;

static void capture(const uint8_t *k, int kl, const uint8_t *v, int vl)
{
    if (g_ncap >= CAP_MAX) return;
    snprintf(g_cap[g_ncap].key, sizeof g_cap[g_ncap].key, "%c:%.*s",
             g_cur, kl, (const char *)k);
    snprintf(g_cap[g_ncap].val, sizeof g_cap[g_ncap].val, "%.*s",
             vl, (const char *)v);
    g_ncap++;
}

static const char *captured(const char *key)
{
    for (int i = 0; i < g_ncap; i++)
        if (strcmp(g_cap[i].key, key) == 0) return g_cap[i].val;
    return "(missing)";
}

/* ── assertions ───────────────────────────────────────────────────────── */

static int g_fail;

static void expect_str(const char *what, const char *got, const char *want)
{
    int ok = strcmp(got, want) == 0;
    printf("  %s %s: got \"%s\"%s\n", ok ? "ok  " : "FAIL", what, got,
           ok ? "" : " (want it to match)");
    if (!ok) { printf("        want \"%s\"\n", want); g_fail = 1; }
}

static void expect_int(const char *what, long got, long want)
{
    int ok = got == want;
    printf("  %s %s: got %ld%s\n", ok ? "ok  " : "FAIL", what, got, ok ? "" : "");
    if (!ok) { printf("        want %ld\n", want); g_fail = 1; }
}

/* ── trace sink (global, multiplexed by g_cur) ────────────────────────── */

static int g_enter_A, g_enter_B;   /* FUNC_ENTER counts per instance */
static int g_name_f_A;             /* NAME events for "f" during A's runs */
static int g_arm_stop_B;           /* return "stop" on B's next FUNC_ENTER */

static int on_event(int kind, const uint8_t *p, int len, void *user)
{
    (void)user;
    if (kind == 0 && g_cur == 'A') {           /* NAME: [u32 atom][u16 n][bytes] */
        uint16_t n = (uint16_t)(p[4] | p[5] << 8);
        if (n == 1 && len >= 7 && p[6] == 'f')
            g_name_f_A++;
    }
    if (kind == 1) {                           /* FUNC_ENTER */
        if (g_cur == 'A') g_enter_A++;
        if (g_cur == 'B') {
            g_enter_B++;
            if (g_arm_stop_B) return 1;        /* host-requested stop, B only */
        }
    }
    return 0;
}

/* ── replay host (global, nests B runs inside A's) ────────────────────── */

static const char *SIM_SRC =
    "const ns = __arena_entry_ns();\n"
    "kv.set('sim_ns', typeof ns);\n"
    "kv.set('sim_date', String(Date.now()));\n"
    "kv.set('sim_rand', String(Math.random()));\n";

static const char *STOPPED_SRC =
    "function g_(){ return 1; }\n"
    "g_();\n";

static uint8_t *dup_cstr(const char *s, int *out_len)
{
    int n = (int)strlen(s);
    uint8_t *p = malloc((size_t)n);
    if (p) memcpy(p, s, n);
    *out_len = n;
    return p;
}

static int kv_get(const uint8_t *key, int key_len,
                  int *out_outcome, uint8_t **out_val, int *out_val_len,
                  void *user)
{
    (void)user;
    char rcbuf[16];

    if (key_len == 4 && memcmp(key, "nest", 4) == 0) {
        /* The saga shape: mid-run on A, synchronously run a module on B. */
        char prev = g_cur;
        g_cur = 'B';
        int rc = arena_run_module_r(g_B, "sim.js", SIM_SRC);
        g_cur = prev;
        snprintf(rcbuf, sizeof rcbuf, "%d", rc);
        *out_val = dup_cstr(rcbuf, out_val_len);
        *out_outcome = 0;
        return 0;
    }
    if (key_len == 9 && memcmp(key, "nest_stop", 9) == 0) {
        /* Nested B run that the host stops after its first event; the
           stop must not leak into A's still-active run. */
        char prev = g_cur;
        g_cur = 'B';
        arena_set_trace_mode_r(g_B, ARENA_TRACE_SCAN);
        g_arm_stop_B = 1;
        int rc = arena_run_module_r(g_B, "stopped.js", STOPPED_SRC);
        g_arm_stop_B = 0;
        arena_set_trace_mode_r(g_B, ARENA_TRACE_OFF);
        g_cur = prev;
        snprintf(rcbuf, sizeof rcbuf, "%d", rc);
        *out_val = dup_cstr(rcbuf, out_val_len);
        *out_outcome = 0;
        return 0;
    }
    *out_outcome = 1;   /* not_found */
    return 0;
}

static int kv_set(const uint8_t *key, int key_len,
                  const uint8_t *val, int val_len, int *out_outcome,
                  void *user)
{
    (void)user;
    capture(key, key_len, val, val_len);
    *out_outcome = 0;
    return 0;
}

/* ── the harness-side module ──────────────────────────────────────────── */

static const char *ENTRY_A =
    "function f(){ return 1; }\n"
    "f();\n"                                        /* NAME 'f' interned */
    "const pre = __arena_entry_ns();\n"
    "kv.set('nest_rc', String(kv.get('nest')));\n"  /* B runs in here */
    "f();\n"                     /* must not re-emit NAME 'f' after the nest */
    "const post = __arena_entry_ns();\n"
    "kv.set('ns_stable', String(pre === post && typeof post === 'object'));\n"
    "kv.set('date_after_nest', String(Date.now()));\n"
    "kv.set('stop_rc', String(kv.get('nest_stop')));\n" /* stopped B run */
    "f();\n"                     /* A's hooks must still fire after B's stop */
    "kv.set('rand', String(Math.random()));\n";

/* Allocation churn: peak live set is one small object, cumulative
   allocation far beyond B's request region. GC regime reclaims and
   completes; bump regime exhausts. */
static const char *CHURN_SRC =
    "let s = 0;\n"
    "for (let i = 0; i < 400000; i++) { s += ({ a: i }).a % 7; }\n"
    "kv.set('churn', String(s));\n";

int main(void)
{
    /* B gets a deliberately small request region so bump mode OOMs. */
    g_A = arena_reactor_new(4096, 2048);
    g_B = arena_reactor_new(4096, 512);
    if (!g_A || !g_B) {
        fprintf(stderr, "arena_reactor_new failed\n");
        return 1;
    }

    arena_replay_host host = { .kv_get = kv_get, .kv_set = kv_set };
    arena_replay_set_host(&host, NULL);
    arena_trace_set_host(on_event, NULL, NULL);

    arena_set_date_now_r(g_A, 1111111);
    arena_set_random_seed_r(g_A, 42);
    arena_set_trace_mode_r(g_A, ARENA_TRACE_SCAN);

    arena_set_date_now_r(g_B, 2222222);
    arena_set_random_seed_r(g_B, 7);
    arena_set_trace_mode_r(g_B, ARENA_TRACE_OFF);

    printf("== nested run (A harness, B sim) ==\n");
    g_cur = 'A';
    int rc_a = arena_run_module_r(g_A, "main.js", ENTRY_A);
    g_cur = '-';

    expect_int("A run rc", rc_a, ARENA_RC_OK);
    expect_str("nested B rc (via A)", captured("A:nest_rc"), "0");
    expect_str("stopped B rc (via A)", captured("A:stop_rc"), "0");
    expect_str("B entry-ns resolves", captured("B:sim_ns"), "object");
    expect_str("A entry-ns stable across nest", captured("A:ns_stable"), "true");
    expect_str("B Date.now pin", captured("B:sim_date"), "2222222");
    expect_str("A Date.now pin after nest", captured("A:date_after_nest"), "1111111");
    expect_int("A/B Math.random differ",
               strcmp(captured("A:rand"), captured("B:sim_rand")) != 0, 1);

    /* Module body enters twice (once more through the async-resume path
       QJS-ng routes module evaluation through for top-level await) plus
       three f() calls. If B's stop had leaked into A's trace state, the
       enters after the nest would be missing and this would come up
       short. */
    expect_int("A FUNC_ENTER count", g_enter_A, 5);
    /* stopped.js: module body enters, host stops there — g_() never runs.
       sim.js ran with tracing OFF, so contributes nothing. */
    expect_int("B FUNC_ENTER count", g_enter_B, 1);
    /* A interned 'f' once; a nested run clearing A's table would have
       forced a duplicate NAME on the post-nest f() calls. */
    expect_int("NAME 'f' emitted once on A", g_name_f_A, 1);

    printf("== per-instance request regime + OOM records ==\n");
    arena_set_request_mode_r(g_B, 1 /* bump */);
    g_cur = 'B';
    int rc_bump = arena_run_module_r(g_B, "churn.js", CHURN_SRC);
    g_cur = 'A';
    int rc_gc = arena_run_module_r(g_A, "churn.js", CHURN_SRC);
    g_cur = '-';

    expect_int("B churn under bump rc", rc_bump, ARENA_RC_OOM);
    expect_int("B oom_hit", arena_oom_hit_r(g_B), 1);
    expect_int("A churn under GC rc", rc_gc, ARENA_RC_OK);
    expect_int("A oom_hit", arena_oom_hit_r(g_A), 0);
    expect_int("B oom_limit sane", arena_oom_limit_r(g_B) > 0, 1);

    arena_reactor_free(g_A);
    arena_reactor_free(g_B);

    printf("== %s ==\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
