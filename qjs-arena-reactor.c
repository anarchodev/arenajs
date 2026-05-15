/*
 * arenajs WASM reactor entry point.
 *
 * Exposes a minimal three-function surface for the browser:
 *   arena_init(base_kb, request_kb)  — set up runtime, context, freeze
 *   arena_run(src)                   — reset request arena, eval, print result
 *   arena_destroy()                  — tear everything down
 *
 * Plain JS host bindings (kv, Math.random, Date.now, crypto.*, module
 * loader) and the trace-event emitter import surface land in follow-on
 * commits — this file is just enough to prove the arena runtime boots
 * under Emscripten.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "qjs-arena.h"
#include "qjs-arena-replay-bindings.h"
#include "qjs-arena-trace.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define ARENA_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define ARENA_EXPORT __attribute__((visibility("default")))
#endif

static JSRuntime *g_rt;
static JSContext *g_ctx;

ARENA_EXPORT
int arena_init(int base_kb, int request_kb)
{
    if (g_rt)
        return -1; /* already initialized */

    size_t base_bytes    = (base_kb    > 0 ? (size_t)base_kb    : 4096) * 1024;
    size_t request_bytes = (request_kb > 0 ? (size_t)request_kb : 4096) * 1024;

    g_rt = JS_NewRuntimeArena(base_bytes, request_bytes);
    if (!g_rt)
        return -1;

    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) {
        JS_FreeRuntime(g_rt);
        g_rt = NULL;
        return -1;
    }

    /* Install replay-tape bindings BEFORE freeze so the binding objects
       and their native function references land in base memory and
       survive per-request resets. */
    if (arena_install_replay_bindings(g_ctx) < 0) {
        JS_FreeContext(g_ctx); g_ctx = NULL;
        JS_FreeRuntime(g_rt);  g_rt  = NULL;
        return -1;
    }

    /* Module loader is set on the runtime; the function-pointer fields
       it stores need to be in base memory, so install pre-freeze too. */
    arena_install_replay_module_loader(g_rt);

    /* Freeze: from here on, per-request allocations land in the request
       arena. Base holds the runtime + default context + replay bindings. */
    JS_FreezeRuntime(g_rt);
    return 0;
}

/* Recognise the host-requested-stop sentinel: a thrown error with
   message exactly ARENA_TRACE_STOP_MSG. arena_run_module turns this
   into a clean rc=0 return rather than the usual rc=-1 error. */
static bool is_trace_stop(JSContext *ctx, JSValueConst exc)
{
    JSValue mv = JS_GetPropertyStr(ctx, exc, "message");
    if (JS_IsException(mv) || JS_IsUndefined(mv)) {
        JS_FreeValue(ctx, mv);
        return false;
    }
    const char *msg = JS_ToCString(ctx, mv);
    JS_FreeValue(ctx, mv);
    bool match = msg && strcmp(msg, ARENA_TRACE_STOP_MSG) == 0;
    if (msg) JS_FreeCString(ctx, msg);
    return match;
}

/* Drain pending microtasks until quiet. Returns -1 if any job threw,
   with its exception printed to stderr; otherwise returns 0. A trace
   stop fired from inside a microtask is converted to a clean rc=0
   exit just like in arena_run_module. */
static int drain_pending_jobs(void)
{
    JSContext *ectx = NULL;
    for (;;) {
        if (!JS_IsJobPending(g_rt))
            return 0;
        int r = JS_ExecutePendingJob(g_rt, &ectx);
        if (r < 0) {
            JSContext *xc = ectx ? ectx : g_ctx;
            JSValue exc = JS_GetException(xc);
            if (is_trace_stop(xc, exc)) {
                JS_FreeValue(xc, exc);
                return 0;
            }
            const char *s = JS_ToCString(xc, exc);
            fprintf(stderr, "pending-job exception: %s\n", s ? s : "(null)");
            if (s) JS_FreeCString(xc, s);
            JS_FreeValue(xc, exc);
            return -1;
        }
    }
}

/* If `v` is a Promise (which is what JS_Eval(MODULE) returns in QJS-ng),
   drain microtasks until it settles, then return its fulfilled value or
   convert a rejection into a thrown exception on `g_ctx`. Takes
   ownership of `v` and returns a value the caller must free.

   If `v` isn't a Promise (e.g. a non-module eval result), pass it back
   unchanged — same ownership semantics. Mirrors quickjs-libc.c's
   js_std_await; reimplemented here so we don't pull in the rest of
   quickjs-libc (which has POSIX deps we'd otherwise have to stub). */
static JSValue await_value(JSValue v)
{
    for (;;) {
        JSPromiseStateEnum st = JS_PromiseState(g_ctx, v);
        if (st == JS_PROMISE_NOT_A_PROMISE)
            return v;
        if (st == JS_PROMISE_FULFILLED) {
            JSValue r = JS_PromiseResult(g_ctx, v);
            JS_FreeValue(g_ctx, v);
            return r;
        }
        if (st == JS_PROMISE_REJECTED) {
            JSValue r = JS_PromiseResult(g_ctx, v);
            JS_FreeValue(g_ctx, v);
            return JS_Throw(g_ctx, r);
        }
        /* PENDING — drain a job; if no jobs left, the promise will
           never settle and we'd loop forever, so bail. */
        if (!JS_IsJobPending(g_rt)) {
            JS_FreeValue(g_ctx, v);
            return JS_ThrowInternalError(g_ctx,
                "module promise pending with no pending jobs to advance it");
        }
        JSContext *ectx = NULL;
        int r = JS_ExecutePendingJob(g_rt, &ectx);
        if (r < 0) {
            /* job threw — surface its exception (it's on ectx's stack;
               move it to g_ctx if different). */
            JSValue exc = JS_GetException(ectx ? ectx : g_ctx);
            JS_FreeValue(g_ctx, v);
            return JS_Throw(g_ctx, exc);
        }
    }
}

ARENA_EXPORT
int arena_run(const char *src)
{
    if (!g_rt || !g_ctx || !src)
        return -1;

    JS_ResetRequestArena(g_rt);

    JSValue v = JS_Eval(g_ctx, src, strlen(src), "<arena>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(g_ctx);
        const char *s = JS_ToCString(g_ctx, exc);
        fprintf(stderr, "arena_run exception: %s\n", s ? s : "(null)");
        if (s) JS_FreeCString(g_ctx, s);
        JS_FreeValue(g_ctx, exc);
        JS_FreeValue(g_ctx, v);
        return -1;
    }

    const char *s = JS_ToCString(g_ctx, v);
    if (s) {
        printf("%s\n", s);
        JS_FreeCString(g_ctx, s);
    }
    JS_FreeValue(g_ctx, v);
    return 0;
}

/* Evaluate `entry_src` as the body of a module named `entry_name`.
   Imports inside it (and transitively) trigger the replay-mode module
   loader, which pulls source from Module.module_sources per
   Module.tapes.module. After eval, microtasks drain so any top-level
   awaits / dynamic import promises settle before returning. */
ARENA_EXPORT
int arena_run_module(const char *entry_name, const char *entry_src)
{
    if (!g_rt || !g_ctx || !entry_name || !entry_src)
        return -1;

    JS_ResetRequestArena(g_rt);
    arena_trace_reset();   /* clear name-table so host can dedupe by run */

    JSValue v = JS_Eval(g_ctx, entry_src, strlen(entry_src), entry_name,
                        JS_EVAL_TYPE_MODULE);
    /* The result is the module's evaluation Promise (QJS-ng returns one
       for every module to support top-level await). Await it so a
       module-body throw surfaces as a real exception instead of getting
       swallowed in a rejected-but-uninspected Promise. */
    v = await_value(v);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(g_ctx);
        /* host_trace returned truthy → trace module raised a sentinel
           InternalError to unwind the stack cleanly. Return 0 so the
           host can distinguish "stopped on purpose" from "errored". */
        if (is_trace_stop(g_ctx, exc)) {
            JS_FreeValue(g_ctx, exc);
            return 0;
        }
        const char *s = JS_ToCString(g_ctx, exc);
        fprintf(stderr, "arena_run_module exception: %s\n", s ? s : "(null)");
        if (s) JS_FreeCString(g_ctx, s);
        JS_FreeValue(g_ctx, exc);
        return -1;
    }
    JS_FreeValue(g_ctx, v);

    return drain_pending_jobs();
}

ARENA_EXPORT
void arena_set_trace_mode(int mode)
{
    arena_trace_set_mode(mode);
}

ARENA_EXPORT
int arena_snapshot_here(void)
{
    return arena_trace_snapshot_here();
}

ARENA_EXPORT
void arena_destroy(void)
{
    if (g_ctx) {
        JS_FreeContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_rt) {
        JS_FreeRuntime(g_rt);
        g_rt = NULL;
    }
}
