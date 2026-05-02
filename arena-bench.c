/*
 * Benchmark: per-request reset speed using a globalThis.blah read/write
 * cycle. Each iteration:
 *   - verifies globalThis.blah is undefined (correctness: shadow was reset)
 *   - sets globalThis.blah = N
 *   - verifies globalThis.blah === N
 *   - JS_ResetRequestArena
 *
 * Reports mean per-iteration time. Also measures the cost of reset by
 * itself (no eval) as a floor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"
#include "qjs-arena.h"

#define ITER_COUNT 50000
#define WARMUP     500

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int eval_check(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<bench>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        fprintf(stderr, "EVAL FAILED: %s\n", s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntimeArena(8 * 1024 * 1024, 8 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 1; }
    JS_FreezeRuntime(rt);

    /* The script verifies the shadow was cleared by reset. If a prior
       request's globalThis.blah leaks across, the first check throws. */
    const char *script =
        "if (globalThis.blah !== undefined) throw 'leak: blah was set';\n"
        "globalThis.blah = 42;\n"
        "if (globalThis.blah !== 42) throw 'set did not stick';\n"
        "'ok'";

    /* Warmup */
    for (int i = 0; i < WARMUP; i++) {
        if (eval_check(ctx, script)) return 1;
        JS_ResetRequestArena(rt);
    }

    /* Measure: eval + reset */
    double t0 = now_ns();
    for (int i = 0; i < ITER_COUNT; i++) {
        if (eval_check(ctx, script)) return 1;
        JS_ResetRequestArena(rt);
    }
    double t1 = now_ns();
    double ns_per_iter = (t1 - t0) / ITER_COUNT;
    printf("eval(script) + reset:  %.0f ns/iter  (%d iters)\n",
           ns_per_iter, ITER_COUNT);

    /* Measure: reset alone (after a state-loaded request).
       Each iteration just resets — no eval between. The request arena
       sits at "just JSRequestState" between resets, so this measures
       the lower-bound cost of reset. */
    if (eval_check(ctx, script)) return 1;
    t0 = now_ns();
    for (int i = 0; i < ITER_COUNT; i++) {
        JS_ResetRequestArena(rt);
    }
    t1 = now_ns();
    double ns_per_reset = (t1 - t0) / ITER_COUNT;
    printf("reset alone:           %.0f ns/iter  (%d iters)\n",
           ns_per_reset, ITER_COUNT);

    /* For perspective: measure eval cost without reset (each iteration
       declares a fresh atom so we exercise the atom overlay). */
    /* Reset once to clean state, then run N evals that write to a new
       global each time. We can't use globalThis.blah (would fail check)
       so use a counter-named global. After N evals we just reset. */
    JS_ResetRequestArena(rt);
    const char *counter_set = "globalThis.x_$ITER = $ITER; 'ok'";
    (void)counter_set;
    /* Skip this for now — would need string formatting. The first two
       measurements are the load-bearing ones. */

    js_dual_arena_free(JS_GetDualArena(rt));
    return 0;
}
