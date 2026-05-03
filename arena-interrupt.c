/*
 * Verify JS_SetInterruptHandler fires for arena runtimes.
 *
 * The bytecode interpreter polls the handler periodically via
 * js_poll_interrupts. In arena mode the per-request counter lives
 * on JSRequestState (not on the base ctx) so the decrement doesn't
 * dirty base. This test runs a tight infinite-loop script under an
 * interrupt handler that aborts after N polls, and asserts:
 *
 *   1. The handler actually fires (counter incremented).
 *   2. Eval returns an Uncatchable Error (interrupt).
 *   3. The thermometer reports zero base bytes touched across the
 *      whole timed-out request.
 *
 * Build: handled by CMake; produces build/arena-interrupt.
 */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
#include "qjs-arena.h"

static int s_polls = 0;
static int s_poll_limit = 50;

static int interrupt_after_n(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    s_polls++;
    return s_polls >= s_poll_limit;
}

int main(void)
{
    JSRuntime *rt  = JS_NewRuntimeArena(16 * 1024 * 1024, 16 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(rt);
    JS_FreezeRuntime(rt);
    JS_SetInterruptHandler(rt, interrupt_after_n, NULL);
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n");
        return 1;
    }

    static const char src[] =
        "(()=>{ try { while (true) {} } catch (e) { return 'caught' } })()";

    int ok = 1;

    for (int iter = 0; iter < 3; iter++) {
        s_polls = 0;
        js_arena_thermometer_reset();

        JSValue v = JS_Eval(ctx, src, sizeof(src) - 1, "<irq>",
                            JS_EVAL_TYPE_GLOBAL);
        size_t pages = js_arena_thermometer_pages();
        size_t bytes = js_arena_thermometer_changed_bytes();

        bool exc = JS_IsException(v);
        bool uncatchable = false;
        if (exc) {
            JSValue e = JS_GetException(ctx);
            uncatchable = JS_IsUncatchableError(e);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, v);

        printf("iter %d: polls=%d exception=%d uncatchable=%d "
               "base pages=%zu bytes=%zu\n",
               iter, s_polls, exc, uncatchable, pages, bytes);

        if (s_polls < s_poll_limit) {
            fprintf(stderr, "  FAIL: handler fired %d times, expected >= %d\n",
                    s_polls, s_poll_limit);
            ok = 0;
        }
        if (!exc || !uncatchable) {
            fprintf(stderr, "  FAIL: expected uncatchable interrupt error\n");
            ok = 0;
        }
        if (pages != 0 || bytes != 0) {
            fprintf(stderr, "  FAIL: base dirtied %zu pages / %zu bytes\n",
                    pages, bytes);
            ok = 0;
        }

        JS_ResetRequestArena(rt);
    }

    js_arena_thermometer_disable();
    js_dual_arena_free(JS_GetDualArena(rt));
    return ok ? 0 : 1;
}
