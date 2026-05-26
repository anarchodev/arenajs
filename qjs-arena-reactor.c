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

/* arena_run / arena_run_module return codes. The caller (the product
   embedding us) must be able to tell "the JS code threw" — a user
   error, surface it, don't retry — from "we ran out of request arena"
   — a capacity signal: the result is void, resize / retry / 503, and
   tell the operator. They are NOT the same outcome and conflating
   them as a bare -1 made the failure reason unrecoverable for the
   caller (made worse by QJS mangling the OOM into a null exception).

     ARENA_RC_OK     0   success, or clean host-requested stop
     ARENA_RC_ERROR -1   JS exception (user error)
     ARENA_RC_OOM   -2   request arena exhausted — result is void;
                         query arena_oom_* for actionable numbers */
#define ARENA_RC_OK     0
#define ARENA_RC_ERROR (-1)
#define ARENA_RC_OOM   (-2)

static int arena_oom_override(int rc, const char *where);

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

/* Diagnostic dump for any exception arena_run_module unexpectedly
   surfaces. The original "just call JS_ToCString(exc)" path was
   useless under arena pressure: when the arena is full the cstring
   alloc itself fails and we get "(null)" with no other context.
   Print each piece independently so a partial failure still tells
   us *something*. Tag identifies the value class; name/message/stack
   come straight off the exception object via direct property lookup;
   the toString fallback runs last because it can also fail. */
static const char *tag_name(int tag)
{
    switch (tag) {
        case JS_TAG_OBJECT:           return "OBJECT";
        case JS_TAG_STRING:           return "STRING";
        case JS_TAG_INT:              return "INT";
        case JS_TAG_FLOAT64:          return "FLOAT64";
        case JS_TAG_BOOL:             return "BOOL";
        case JS_TAG_NULL:             return "NULL";
        case JS_TAG_UNDEFINED:        return "UNDEFINED";
        case JS_TAG_EXCEPTION:        return "EXCEPTION";
        case JS_TAG_UNINITIALIZED:    return "UNINITIALIZED";
        case JS_TAG_SYMBOL:           return "SYMBOL";
        default:                      return "OTHER";
    }
}

static void diagnose_exception(JSContext *ctx, JSValueConst exc, const char *where)
{
    int tag = JS_VALUE_GET_TAG(exc);
    fprintf(stderr, "%s exception: tag=%s", where, tag_name(tag));

    if (JS_IsObject(exc)) {
        const char *fields[] = { "name", "message", "stack", NULL };
        for (int i = 0; fields[i]; i++) {
            JSValue v = JS_GetPropertyStr(ctx, exc, fields[i]);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                fprintf(stderr, " %s=<get-failed>", fields[i]);
                continue;
            }
            if (JS_IsUndefined(v) || JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                continue;
            }
            const char *s = JS_ToCString(ctx, v);
            fprintf(stderr, " %s=%s", fields[i], s ? s : "<cstring-failed>");
            if (s) JS_FreeCString(ctx, s);
            else JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, v);
        }
    }

    const char *toStr = JS_ToCString(ctx, exc);
    fprintf(stderr, " toString=%s\n", toStr ? toStr : "<failed>");
    if (toStr) JS_FreeCString(ctx, toStr);
    else JS_FreeValue(ctx, JS_GetException(ctx));
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
            if (is_trace_stop(xc, exc) || arena_trace_stop_armed()) {
                JS_FreeValue(xc, exc);
                return 0;
            }
            diagnose_exception(xc, exc, "pending-job");
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
        if (js_dual_arena_oom_hit(JS_GetDualArena(g_rt))) {
            JS_FreeValue(g_ctx, exc);
            JS_FreeValue(g_ctx, v);
            return arena_oom_override(ARENA_RC_ERROR, "arena_run");
        }
        diagnose_exception(g_ctx, exc, "arena_run");
        JS_FreeValue(g_ctx, exc);
        JS_FreeValue(g_ctx, v);
        return ARENA_RC_ERROR;
    }

    const char *s = JS_ToCString(g_ctx, v);
    if (s) {
        printf("%s\n", s);
        JS_FreeCString(g_ctx, s);
    }
    JS_FreeValue(g_ctx, v);
    return arena_oom_override(ARENA_RC_OK, "arena_run");
}

/* Evaluate `entry_src` as the body of a module named `entry_name`.
   Imports inside it (and transitively) trigger the replay-mode module
   loader, which pulls source from Module.module_sources per
   Module.tapes.module. After eval, microtasks drain so any top-level
   awaits / dynamic import promises settle before returning. */
/* Conservative OOM policy: if any request-mode allocation was refused
   this run, the result is void regardless of how execution limped to
   its end — some object silently didn't get created. Report
   ARENA_RC_OOM with an actionable line rather than let a
   silently-wrong result through. NOT applied to a clean
   host-requested stop: that's the host explicitly saying "I have what
   I need, tear down", not a tainted result. */
static int arena_oom_override(int rc, const char *where)
{
    if (!g_rt)
        return rc;
    JSDualArena *da = JS_GetDualArena(g_rt);
    if (!js_dual_arena_oom_hit(da))
        return rc;
    fprintf(stderr,
            "%s: request arena exhausted — needed %zu B, %zu / %zu B used\n",
            where,
            js_dual_arena_oom_requested(da),
            js_dual_arena_oom_used(da),
            js_dual_arena_oom_limit(da));
    return ARENA_RC_OOM;
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
        return ARENA_RC_ERROR;

    JS_ResetRequestArena(g_rt);   /* also clears the per-request OOM record */
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
           InternalError to unwind the stack cleanly. Two flavours:
           (a) is_trace_stop matches the sentinel message → normal case.
           (b) the sentinel's InternalError construction itself OOM-d
               on arena pressure, exception value ended up JS_NULL,
               but s_bail_armed in the trace module records that stop
               was requested. Treat as clean stop — do NOT reclassify
               as OOM, the host asked to stop on purpose. */
        if (is_trace_stop(g_ctx, exc) || arena_trace_stop_armed()) {
            JS_FreeValue(g_ctx, exc);
            return ARENA_RC_OK;
        }
        /* Arena-exhaustion is the likely cause when the exception is
           the mangled JS_NULL; report it as OOM with numbers rather
           than the generic (and now redundant) diagnose path. */
        if (js_dual_arena_oom_hit(JS_GetDualArena(g_rt))) {
            JS_FreeValue(g_ctx, exc);
            return arena_oom_override(ARENA_RC_ERROR, "arena_run_module");
        }
        diagnose_exception(g_ctx, exc, "arena_run_module");
        JS_FreeValue(g_ctx, exc);
        return ARENA_RC_ERROR;
    }
    JS_FreeValue(g_ctx, v);

    /* Even a "successful" run is void if it touched the ceiling. */
    return arena_oom_override(drain_pending_jobs(), "arena_run_module");
}

ARENA_EXPORT int arena_oom_hit(void)
{
    return g_rt ? (js_dual_arena_oom_hit(JS_GetDualArena(g_rt)) ? 1 : 0) : 0;
}
ARENA_EXPORT double arena_oom_requested(void)
{
    return g_rt ? (double)js_dual_arena_oom_requested(JS_GetDualArena(g_rt)) : 0;
}
ARENA_EXPORT double arena_oom_used(void)
{
    return g_rt ? (double)js_dual_arena_oom_used(JS_GetDualArena(g_rt)) : 0;
}
ARENA_EXPORT double arena_oom_limit(void)
{
    return g_rt ? (double)js_dual_arena_oom_limit(JS_GetDualArena(g_rt)) : 0;
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

/* Seed the per-request PRNG used by Math.random (and, via
 * JS_FillRandomBytes, by host crypto bindings). Splits a u64 into
 * two u32s so the WASM ABI doesn't need BigInt — the JS host calls
 * arena_set_random_seed(seed_lo, seed_hi) with the recorded request
 * seed at replay start. The native server path also calls
 * JS_SetRandomSeed directly with the per-request seed (one path,
 * one PRNG state — see docs/primitive-gaps.md §9). */
ARENA_EXPORT
void arena_set_random_seed(uint32_t seed_lo, uint32_t seed_hi)
{
    if (!g_ctx) return;
    uint64_t seed = ((uint64_t)seed_hi << 32) | seed_lo;
    JS_SetRandomSeed(g_ctx, seed);
}

/* Pin Date.now() to a fixed UTC-ms-since-epoch value for the next
 * arena_run. Within one request, every Date.now() and
 * `new Date()` (no args) returns this value — making the per-
 * request clock-read sequence deterministic.  The 64-bit ms value
 * is split across two u32s so the WASM ABI doesn't need BigInt
 * (replay shell passes (lo, hi) from the captured timestamp).
 * The server path calls JS_SetDateNow directly with the per-
 * request value — one mechanism, one source.  See
 * docs/primitive-gaps.md §9 fold-in in rove. */
ARENA_EXPORT
void arena_set_date_now(uint32_t lo, uint32_t hi)
{
    if (!g_ctx) return;
    int64_t ms = (int64_t)(((uint64_t)hi << 32) | lo);
    JS_SetDateNow(g_ctx, ms);
}
