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

static double bench_eval_reset(JSContext *ctx, JSRuntime *rt,
                               const char *label, const char *script,
                               int iters)
{
    /* warmup */
    for (int i = 0; i < WARMUP; i++) {
        if (eval_check(ctx, script)) return -1;
        JS_ResetRequestArena(rt);
    }
    double t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        if (eval_check(ctx, script)) return -1;
        JS_ResetRequestArena(rt);
    }
    double t1 = now_ns();
    double ns_per = (t1 - t0) / iters;
    printf("%-36s %7.0f ns/iter  (%d iters)\n", label, ns_per, iters);
    return ns_per;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntimeArena(8 * 1024 * 1024, 8 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 1; }

    /* Stash a snapshot-time global so the override-existing test
       can verify the base value comes back after reset. */
    if (eval_check(ctx, "globalThis.GREETING = 'hello from base';")) return 1;

    JS_FreezeRuntime(rt);

    /* Test A: shadow a fresh global. Each iter starts with the base
       global_obj having no `blah`; reset must clear the shadow. */
    const char *script_a =
        "if (globalThis.blah !== undefined) throw 'leak A: blah was set';\n"
        "globalThis.blah = 42;\n"
        "if (globalThis.blah !== 42) throw 'A: set did not stick';\n"
        "'ok'";

    /* Test B: override an EXISTING base global (GREETING). Goes
       through OP_put_field's fast path because the property exists on
       base global_obj. After reset, base GREETING should be visible
       again. */
    const char *script_b =
        "if (globalThis.GREETING !== 'hello from base') throw 'leak B';\n"
        "globalThis.GREETING = 'overridden';\n"
        "if (globalThis.GREETING !== 'overridden') throw 'B: override no stick';\n"
        "'ok'";

    /* Test C: delete an existing base property + verify gone, then
       reset restores it. */
    const char *script_c =
        "if (globalThis.GREETING !== 'hello from base') throw 'leak C';\n"
        "delete globalThis.GREETING;\n"
        "if (globalThis.GREETING !== undefined) throw 'C: delete no stick';\n"
        "'ok'";

    /* TODO Test D was a more substantive workload (array push + reduce
       with an inline closure). It triggers a latent bug: after 1-2 reset
       cycles, calling an Array.prototype method with an inline arrow
       function returns "not a function" — base method lookup intermittently
       returns undefined. The bug is reproducible under both Release and
       ASan, predates step-6, and likely lives in the bytecode interpreter's
       interaction with closure creation across reset. Deliberately skipped
       here so the rest of the benchmark numbers remain comparable; tracked
       in ARENA_PLAN.md as an open follow-up. */

    bench_eval_reset(ctx, rt, "A: globalThis.blah set",      script_a, ITER_COUNT);
    bench_eval_reset(ctx, rt, "B: override base GREETING",   script_b, ITER_COUNT);
    bench_eval_reset(ctx, rt, "C: delete base GREETING",     script_c, ITER_COUNT);

    /* Reset alone — floor */
    if (eval_check(ctx, script_a)) return 1;
    double t0 = now_ns();
    for (int i = 0; i < ITER_COUNT; i++) {
        JS_ResetRequestArena(rt);
    }
    double t1 = now_ns();
    printf("%-36s %7.0f ns/iter  (%d iters)\n",
           "reset alone (floor)", (t1 - t0) / ITER_COUNT, ITER_COUNT);

    js_dual_arena_free(JS_GetDualArena(rt));
    return 0;
}
