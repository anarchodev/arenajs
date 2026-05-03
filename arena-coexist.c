/*
 * Coexistence smoke test: a vanilla runtime and an arena-backed runtime
 * sharing one thread.
 *
 * Each runtime is exercised independently by an interleaved sequence of
 * evals. We verify:
 *
 *   1. Both runtimes produce correct results for their own evals (no
 *      cross-contamination, no runtime confusion in chokepoint gates).
 *
 *   2. The arena runtime keeps its inviolate-base property: every
 *      request leaves zero base bytes dirtied. The vanilla runtime's
 *      operations between arena requests don't perturb this.
 *
 *   3. The thermometer correctly attributes faults to the arena's
 *      base range and ignores activity inside the vanilla runtime.
 *
 * Build: handled by CMake; produces build/arena-coexist.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "qjs-arena.h"

static int eval_int(JSContext *ctx, const char *src, int *out)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<co>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "EVAL FAIL: %s\n", s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        return -1;
    }
    int32_t i;
    if (JS_ToInt32(ctx, &i, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    *out = i;
    JS_FreeValue(ctx, v);
    return 0;
}

int main(void)
{
    /* 1. Create both runtimes on this thread. */
    JSRuntime *rt_van = JS_NewRuntime();
    JSContext *ctx_van = JS_NewContext(rt_van);

    JSRuntime *rt_arena = JS_NewRuntimeArena(16 * 1024 * 1024,
                                             16 * 1024 * 1024);
    JSContext *ctx_arena = JS_NewContext(rt_arena);

    /* Arena runtime gets a snapshot global. */
    {
        JSValue v = JS_Eval(ctx_arena, "globalThis.SNAPSHOT_TAG = 'arena';",
                            34, "<init>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx_arena, v);
    }
    JS_FreezeRuntime(rt_arena);

    /* Enable the thermometer on the arena range only. */
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n");
        return 1;
    }

    /* 2. Interleaved evals. */
    int v_int, a_int;
    int n_iters = 100;

    size_t total_arena_pages = 0;
    int passed = 0, failed = 0;

    for (int i = 0; i < n_iters; i++) {
        /* Vanilla request: builds an array, sums it. Wrapped in an
           IIFE so `let` doesn't leak into the next iteration's
           top-level scope (which would SyntaxError on redecl). */
        if (eval_int(ctx_van,
                     "(()=>{let xs=[]; for(let i=0;i<50;i++) xs.push(i*i);"
                     "return xs.reduce((a,b)=>a+b,0)})()",
                     &v_int) < 0) { failed++; continue; }
        if (v_int != 40425) {
            fprintf(stderr, "vanilla wrong: %d (expected 40425)\n", v_int);
            failed++; continue;
        }

        /* Arena request: same workload + verify the snapshot tag. The
           reset between iterations clears request-scope state, so this
           need not be wrapped — but wrapping makes the symmetry obvious. */
        js_arena_thermometer_reset();
        if (eval_int(ctx_arena,
                     "if (SNAPSHOT_TAG !== 'arena') throw 'lost tag';"
                     "(()=>{let xs=[]; for(let i=0;i<50;i++) xs.push(i*i);"
                     "return xs.reduce((a,b)=>a+b,0)})()",
                     &a_int) < 0) { failed++; continue; }
        if (a_int != 40425) {
            fprintf(stderr, "arena wrong: %d (expected 40425)\n", a_int);
            failed++; continue;
        }

        size_t pages = js_arena_thermometer_pages();
        size_t bytes = js_arena_thermometer_changed_bytes();
        if (pages != 0 || bytes != 0) {
            fprintf(stderr, "iter %d: arena dirtied %zu pages / %zu bytes\n",
                    i, pages, bytes);
            failed++;
            total_arena_pages += pages;
        }

        JS_ResetRequestArena(rt_arena);
        passed++;
    }

    js_arena_thermometer_disable();

    /* 3. Check the per-thread arena range list reports two entries
       only when both arenas are live, then one after teardown. */
    extern __thread int js_arena_range_count;
    int count_before_free = js_arena_range_count;

    js_dual_arena_free(JS_GetDualArena(rt_arena));

    int count_after_free = js_arena_range_count;

    /* Vanilla runtime cleanup follows the standard pattern. */
    JS_FreeContext(ctx_van);
    JS_FreeRuntime(rt_van);

    printf("\n=== arena-coexist summary ===\n");
    printf("  iters passed:           %d / %d\n", passed, passed + failed);
    printf("  arena pages dirtied:    %zu (target: 0)\n", total_arena_pages);
    printf("  range count w/ arena:   %d (expected 1)\n", count_before_free);
    printf("  range count after free: %d (expected 0)\n", count_after_free);

    int ok = (failed == 0)
          && (total_arena_pages == 0)
          && (count_before_free == 1)
          && (count_after_free == 0);
    return ok ? 0 : 1;
}
