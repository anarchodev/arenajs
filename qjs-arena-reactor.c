/*
 * arenajs reactor implementation. See qjs-arena-reactor.h for the API
 * and the multi-instance contract; this file provides the instance
 * machinery plus the frozen singleton ABI as thin wrappers over one
 * default instance.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "qjs-arena.h"
#include "qjs-arena-reactor.h"
#include "qjs-arena-replay-bindings.h"
#include "qjs-arena-trace.h"

#if !ARENA_TRACE_ENABLED
/* The reactor embeds ArenaTraceState and drives the emitter lifecycle;
   it has never linked against a trace-disabled build (the worker
   engine links qjs-arena.c directly, without this TU). Fail loudly
   rather than at link time. */
#error "qjs-arena-reactor.c requires ARENA_TRACE_ENABLED=1"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* KEEPALIVE force-exports a symbol IN ADDITION to -sEXPORTED_FUNCTIONS,
   so it goes on the frozen singleton ABI only. The instance API is
   native-only surface and must not widen the wasm export list. */
#define ARENA_EXPORT EMSCRIPTEN_KEEPALIVE
#define ARENA_API
#else
#define ARENA_EXPORT __attribute__((visibility("default")))
#define ARENA_API    __attribute__((visibility("default")))
#endif

/* One reactor instance: a frozen base (runtime + context + replay
   bindings) plus everything a run needs that must survive the
   per-request reset. Lives on the libc heap — NOT in either arena —
   so its fields are freely mutable under a hardened (PROT_READ) base. */
struct ArenaReactor {
    JSRuntime *rt;
    JSContext *ctx;

    /* Pending determinism pins. The engine stores the live values in
       JSRequestState, which every per-request reset re-initializes to
       defaults (unpinned clock, zero PRNG state) — but the documented
       order is "set, then run", and arena_run* resets FIRST
       (results/trace must stay readable between runs). So the reactor
       buffers the latest host-set values here and re-applies them
       right after each reset. Sticky until overwritten. */
    bool     has_random_seed;
    uint64_t random_seed;
    bool     has_date_now;
    int64_t  date_now_ms;

    /* Per-instance trace-emitter state, bound to rt pre-freeze so the
       interpreter hooks can resolve it from any of this runtime's
       contexts. */
    ArenaTraceState trace;

    /* The entry module of the CURRENT arena_run_module call. Lives in
       the request arena — valid only between the compile inside one
       arena_run_module call and the next request-arena reset. Consumed
       by `__arena_entry_ns()` (replay bindings), which lets an appended
       replay epilogue reach the entry module's namespace — the only way
       to invoke an anonymous `export default` (no module-scope binding
       exists for it, and both static and dynamic self-imports route
       through the host loader, diverging from the module tape). */
    JSModuleDef *entry_module;

    /* Freeze latch. arena_reactor_new freezes eagerly; arena_reactor_new_open
       leaves the base writable so an embedder can install extra
       natives/globals, and arena_reactor_freeze (idempotent) seals it. */
    bool frozen;
};

static void arena_reapply_pins(ArenaReactor *r)
{
    if (r->has_random_seed)
        JS_SetRandomSeed(r->ctx, r->random_seed);
    if (r->has_date_now)
        JS_SetDateNow(r->ctx, r->date_now_ms);
}

ARENA_API
/* Defined below (the exception diagnostic dump); forward-declared so
   arena_reactor_eval_base can use it. */
static void diagnose_exception(JSContext *ctx, JSValueConst exc, const char *where);

/* The default base-setup: the replay host surface (kv/crypto + the tape
   module loader). arena_reactor_new / arena_reactor_new_open install this,
   so existing embedders (the native sim, the WASM singleton) are unchanged
   by the parameterization. */
static int default_replay_setup(JSContext *ctx, JSRuntime *rt, void *user)
{
    (void)user;
    if (arena_install_replay_bindings(ctx) < 0)
        return -1;
    arena_install_replay_module_loader(rt);
    return 0;
}

/* Build a reactor but leave the base UNFROZEN — the embedder may install
   additional natives / eval globals via arena_reactor_eval_base, then must
   call arena_reactor_freeze before the first run. The `setup` hook installs
   the embedder's base surface (bindings + module loader + any runtime hooks)
   on the fresh (ctx, rt) pre-freeze — see the header. Only the final freeze
   is deferred. */
ArenaReactor *arena_reactor_new_open_with(int base_kb, int request_kb,
                                          arena_base_setup_fn setup, void *user)
{
    ArenaReactor *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    arena_trace_state_init(&r->trace);

    size_t base_bytes    = (base_kb    > 0 ? (size_t)base_kb    : 4096) * 1024;
    size_t request_bytes = (request_kb > 0 ? (size_t)request_kb : 4096) * 1024;

    r->rt = JS_NewRuntimeArena(base_bytes, request_bytes);
    if (!r->rt) {
        free(r);
        return NULL;
    }

    r->ctx = JS_NewContext(r->rt);
    if (!r->ctx) {
        JS_FreeRuntime(r->rt);
        free(r);
        return NULL;
    }

    /* Embedder installs its base surface (native bindings + module loader +
       any runtime hooks) on the fresh (ctx, rt) BEFORE freeze, so the
       binding objects and their function-pointer references land in base
       memory and survive per-request resets. A miss tears the half-built
       reactor down. A NULL setup builds a bare runtime (intrinsics only). */
    if (setup && setup(r->ctx, r->rt, user) < 0) {
        JS_FreeContext(r->ctx);
        JS_FreeRuntime(r->rt);
        free(r);
        return NULL;
    }

    /* Bind instance state to the runtime — BOTH are base-memory writes
       and must stay above JS_FreezeRuntime (harden discipline: rt lives
       in base, which may become PROT_READ). The opaque lets
       arena_entry_module() find this instance from any JSContext; the
       trace slot lets the interpreter hooks find their emitter state.
       Neither pointer is ever rewritten; the pointed-to struct is libc
       heap and stays mutable. */
    JS_SetRuntimeOpaque(r->rt, r);
    js_arena_trace_set_state(r->rt, &r->trace);

    /* NOT frozen yet — the caller (arena_reactor_new, or an embedder that
       wants to add more base globals) seals it with arena_reactor_freeze. */
    return r;
}

/* Unfrozen reactor with the default replay base-setup. Kept for existing
   embedders (native sim, WASM singleton) that want the replay host surface;
   new embedders wanting their own surface use arena_reactor_new_open_with. */
ArenaReactor *arena_reactor_new_open(int base_kb, int request_kb)
{
    return arena_reactor_new_open_with(base_kb, request_kb,
                                       default_replay_setup, NULL);
}

/* Eval `src` as a classic script into the UNFROZEN base — for an embedder to
   install extra globals (e.g. a `_system` prelude + composed shims) that must
   live in base memory and survive per-request resets. Pre-freeze, all
   allocations land in base; pending jobs drain before returning. Returns
   ARENA_RC_*. Refuses (ARENA_RC_ERROR) once frozen — base may be PROT_READ. */
ARENA_API
int arena_reactor_eval_base(ArenaReactor *r, const char *src)
{
    if (!r || !r->rt || !r->ctx || !src || r->frozen)
        return ARENA_RC_ERROR;

    JSValue v = JS_Eval(r->ctx, src, strlen(src), "<arena-base>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(r->ctx);
        diagnose_exception(r->ctx, exc, "arena_reactor_eval_base");
        JS_FreeValue(r->ctx, exc);
        JS_FreeValue(r->ctx, v);
        return ARENA_RC_ERROR;
    }
    JS_FreeValue(r->ctx, v);

    /* Drain any microtasks the install queued (IIFE shims shouldn't, but be
       safe — same discipline as arena_run_module). */
    JSContext *cx;
    int rc;
    while ((rc = JS_ExecutePendingJob(r->rt, &cx)) != 0) {
        if (rc < 0) {
            JSValue exc = JS_GetException(cx);
            diagnose_exception(cx, exc, "arena_reactor_eval_base(job)");
            JS_FreeValue(cx, exc);
            return ARENA_RC_ERROR;
        }
    }
    return ARENA_RC_OK;
}

/* Freeze the base: from here on, per-request allocations land in the request
   arena. Idempotent — a second call is a no-op. Required before the first run
   of an arena_reactor_new_open reactor (arena_reactor_new does it for you). */
void arena_reactor_freeze(ArenaReactor *r)
{
    if (!r || r->frozen)
        return;
    JS_FreezeRuntime(r->rt);
    r->frozen = true;
}

/* Build a reactor and freeze immediately — the original one-shot constructor.
   Base holds the runtime + default context + replay bindings. */
ArenaReactor *arena_reactor_new(int base_kb, int request_kb)
{
    ArenaReactor *r = arena_reactor_new_open(base_kb, request_kb);
    if (!r)
        return NULL;
    arena_reactor_freeze(r);
    return r;
}

ARENA_API
void arena_reactor_free(ArenaReactor *r)
{
    if (!r)
        return;
    if (r->ctx)
        JS_FreeContext(r->ctx);
    if (r->rt)
        JS_FreeRuntime(r->rt);
    free(r);
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
static int drain_pending_jobs(ArenaReactor *r)
{
    JSContext *ectx = NULL;
    for (;;) {
        if (!JS_IsJobPending(r->rt))
            return 0;
        int ret = JS_ExecutePendingJob(r->rt, &ectx);
        if (ret < 0) {
            JSContext *xc = ectx ? ectx : r->ctx;
            JSValue exc = JS_GetException(xc);
            if (is_trace_stop(xc, exc) || arena_trace_stop_armed(&r->trace)) {
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
   convert a rejection into a thrown exception on the instance context.
   Takes ownership of `v` and returns a value the caller must free.

   If `v` isn't a Promise (e.g. a non-module eval result), pass it back
   unchanged — same ownership semantics. Mirrors quickjs-libc.c's
   js_std_await; reimplemented here so we don't pull in the rest of
   quickjs-libc (which has POSIX deps we'd otherwise have to stub). */
static JSValue await_value(ArenaReactor *r, JSValue v)
{
    JSContext *ctx = r->ctx;
    for (;;) {
        JSPromiseStateEnum st = JS_PromiseState(ctx, v);
        if (st == JS_PROMISE_NOT_A_PROMISE)
            return v;
        if (st == JS_PROMISE_FULFILLED) {
            JSValue res = JS_PromiseResult(ctx, v);
            JS_FreeValue(ctx, v);
            return res;
        }
        if (st == JS_PROMISE_REJECTED) {
            JSValue res = JS_PromiseResult(ctx, v);
            JS_FreeValue(ctx, v);
            return JS_Throw(ctx, res);
        }
        /* PENDING — drain a job; if no jobs left, the promise will
           never settle and we'd loop forever, so bail. */
        if (!JS_IsJobPending(r->rt)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowInternalError(ctx,
                "module promise pending with no pending jobs to advance it");
        }
        JSContext *ectx = NULL;
        int ret = JS_ExecutePendingJob(r->rt, &ectx);
        if (ret < 0) {
            /* job threw — surface its exception (it's on ectx's stack;
               move it to the instance context if different). */
            JSValue exc = JS_GetException(ectx ? ectx : ctx);
            JS_FreeValue(ctx, v);
            return JS_Throw(ctx, exc);
        }
    }
}

/* Conservative OOM policy: if any request-mode allocation was refused
   this run, the result is void regardless of how execution limped to
   its end — some object silently didn't get created. Report
   ARENA_RC_OOM with an actionable line rather than let a
   silently-wrong result through. NOT applied to a clean
   host-requested stop: that's the host explicitly saying "I have what
   I need, tear down", not a tainted result. */
static int arena_oom_override(ArenaReactor *r, int rc, const char *where)
{
    if (!r->rt)
        return rc;
    JSDualArena *da = JS_GetDualArena(r->rt);
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

ARENA_API
int arena_run_r(ArenaReactor *r, const char *src)
{
    if (!r || !r->rt || !r->ctx || !src)
        return ARENA_RC_ERROR;

    JS_ResetRequestArena(r->rt);
    arena_reapply_pins(r);

    JSValue v = JS_Eval(r->ctx, src, strlen(src), "<arena>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(r->ctx);
        if (js_dual_arena_oom_hit(JS_GetDualArena(r->rt))) {
            JS_FreeValue(r->ctx, exc);
            JS_FreeValue(r->ctx, v);
            return arena_oom_override(r, ARENA_RC_ERROR, "arena_run");
        }
        diagnose_exception(r->ctx, exc, "arena_run");
        JS_FreeValue(r->ctx, exc);
        JS_FreeValue(r->ctx, v);
        return ARENA_RC_ERROR;
    }

    const char *s = JS_ToCString(r->ctx, v);
    if (s) {
        printf("%s\n", s);
        JS_FreeCString(r->ctx, s);
    }
    JS_FreeValue(r->ctx, v);
    return arena_oom_override(r, ARENA_RC_OK, "arena_run");
}

/* The instance whose arena_run_module call is executing on ctx's
   runtime — resolved through the runtime opaque the reactor claimed at
   build time, so sibling instances (and nested runs across them) each
   see their own entry module. */
JSModuleDef *arena_entry_module(JSContext *ctx)
{
    ArenaReactor *r = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return r ? r->entry_module : NULL;
}

/* Evaluate `entry_src` as the body of a module named `entry_name`.
   Imports inside it (and transitively) trigger the replay-mode module
   loader, which pulls source from Module.module_sources per
   Module.tapes.module. After eval, microtasks drain so any top-level
   awaits / dynamic import promises settle before returning. */
ARENA_API
int arena_run_module_r(ArenaReactor *r, const char *entry_name,
                       const char *entry_src)
{
    if (!r || !r->rt || !r->ctx || !entry_name || !entry_src)
        return ARENA_RC_ERROR;

    r->entry_module = NULL;       /* previous run's def died with its arena */
    JS_ResetRequestArena(r->rt);  /* also clears the per-request OOM record */
    arena_reapply_pins(r);
    arena_trace_reset(&r->trace); /* clear name-table so host can dedupe by run */

    /* Compile-only first so the JSModuleDef is reachable for
       __arena_entry_ns(); JS_EvalFunction then links + evaluates the
       same instance (and consumes the wrapper value). */
    JSValue compiled = JS_Eval(r->ctx, entry_src, strlen(entry_src), entry_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        JSValue exc = JS_GetException(r->ctx);
        if (js_dual_arena_oom_hit(JS_GetDualArena(r->rt))) {
            JS_FreeValue(r->ctx, exc);
            return arena_oom_override(r, ARENA_RC_ERROR, "arena_run_module(compile)");
        }
        diagnose_exception(r->ctx, exc, "arena_run_module(compile)");
        JS_FreeValue(r->ctx, exc);
        return ARENA_RC_ERROR;
    }
    r->entry_module = JS_VALUE_GET_PTR(compiled);

    JSValue v = JS_EvalFunction(r->ctx, compiled);
    /* The result is the module's evaluation Promise (QJS-ng returns one
       for every module to support top-level await). Await it so a
       module-body throw surfaces as a real exception instead of getting
       swallowed in a rejected-but-uninspected Promise. */
    v = await_value(r, v);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(r->ctx);
        /* host_trace returned truthy → trace module raised a sentinel
           InternalError to unwind the stack cleanly. Two flavours:
           (a) is_trace_stop matches the sentinel message → normal case.
           (b) the sentinel's InternalError construction itself OOM-d
               on arena pressure, exception value ended up JS_NULL,
               but bail_armed in this instance's trace state records
               that stop was requested. Treat as clean stop — do NOT
               reclassify as OOM, the host asked to stop on purpose. */
        if (is_trace_stop(r->ctx, exc) || arena_trace_stop_armed(&r->trace)) {
            JS_FreeValue(r->ctx, exc);
            return ARENA_RC_OK;
        }
        /* Arena-exhaustion is the likely cause when the exception is
           the mangled JS_NULL; report it as OOM with numbers rather
           than the generic (and now redundant) diagnose path. */
        if (js_dual_arena_oom_hit(JS_GetDualArena(r->rt))) {
            JS_FreeValue(r->ctx, exc);
            return arena_oom_override(r, ARENA_RC_ERROR, "arena_run_module");
        }
        diagnose_exception(r->ctx, exc, "arena_run_module");
        JS_FreeValue(r->ctx, exc);
        return ARENA_RC_ERROR;
    }
    JS_FreeValue(r->ctx, v);

    /* Even a "successful" run is void if it touched the ceiling. */
    return arena_oom_override(r, drain_pending_jobs(r), "arena_run_module");
}

ARENA_API
void arena_set_trace_mode_r(ArenaReactor *r, int mode)
{
    if (!r) return;
    r->trace.mode = mode;
}

/* Select the request-allocator regime for this instance's SUBSEQUENT
 * runs (qjs-arena.h JSArenaReqMode: 0 = GC mspace, 1 = bump). Records
 * the choice on the dual arena; it binds at the next request-arena
 * reset — which arena_run*_r perform at entry — so call this between
 * runs, before the run it should govern. Replay embedders use it to
 * re-run a request under the regime the live request completed under
 * (a GC-completed churny request would OOM under bump). */
ARENA_API
void arena_set_request_mode_r(ArenaReactor *r, int mode)
{
    if (!r || !r->rt) return;
    JSDualArena *da = JS_GetDualArena(r->rt);
    js_dual_arena_set_request_mode(
        da, mode == 0 ? JS_ARENA_REQ_MODE_GC : JS_ARENA_REQ_MODE_BUMP);
}

/* Seed the per-request PRNG used by Math.random (and, via
 * JS_FillRandomBytes, by host crypto bindings). Sticky: re-applied
 * after every per-request reset until overwritten. */
ARENA_API
void arena_set_random_seed_r(ArenaReactor *r, uint64_t seed)
{
    if (!r || !r->ctx) return;
    r->random_seed = seed;
    r->has_random_seed = true;
    JS_SetRandomSeed(r->ctx, seed);
}

/* Pin Date.now() (and no-arg `new Date()`) to a fixed UTC-ms value for
 * this instance's runs. Sticky, like the PRNG seed. */
ARENA_API
void arena_set_date_now_r(ArenaReactor *r, int64_t ms)
{
    if (!r || !r->ctx) return;
    r->date_now_ms = ms;
    r->has_date_now = true;
    JS_SetDateNow(r->ctx, ms);
}

void arena_set_interrupt_r(ArenaReactor *r, JSInterruptHandler *cb, void *opaque)
{
    if (!r || !r->rt) return;
    JS_SetInterruptHandler(r->rt, cb, opaque);
}

ARENA_API
int arena_oom_hit_r(ArenaReactor *r)
{
    return (r && r->rt) ? (js_dual_arena_oom_hit(JS_GetDualArena(r->rt)) ? 1 : 0) : 0;
}
ARENA_API
size_t arena_oom_requested_r(ArenaReactor *r)
{
    return (r && r->rt) ? js_dual_arena_oom_requested(JS_GetDualArena(r->rt)) : 0;
}
ARENA_API
size_t arena_oom_used_r(ArenaReactor *r)
{
    return (r && r->rt) ? js_dual_arena_oom_used(JS_GetDualArena(r->rt)) : 0;
}
ARENA_API
size_t arena_oom_limit_r(ArenaReactor *r)
{
    return (r && r->rt) ? js_dual_arena_oom_limit(JS_GetDualArena(r->rt)) : 0;
}

/* ── singleton wrappers ─────────────────────────────────────────────
 * The frozen WASM/CLI ABI: one default instance, unchanged names and
 * signatures (u32 lo/hi splits and double returns are wasm32-ABI
 * concessions — no BigInt at the boundary). */

static ArenaReactor *g_default;

/* arena_set_trace_mode historically worked before arena_init (it set
   the bare emitter global, which the first run then honoured). With
   per-instance state there is no global to set, so buffer the last
   pre-init value and seed it into the default instance on creation. */
static int g_pending_trace_mode = ARENA_TRACE_OFF;

ARENA_EXPORT
int arena_init(int base_kb, int request_kb)
{
    if (g_default)
        return -1; /* already initialized */
    g_default = arena_reactor_new(base_kb, request_kb);
    if (!g_default)
        return -1;
    arena_set_trace_mode_r(g_default, g_pending_trace_mode);
    return 0;
}

ARENA_EXPORT
int arena_init_open(int base_kb, int request_kb)
{
    if (g_default)
        return -1; /* already initialized */
    g_default = arena_reactor_new_open(base_kb, request_kb);
    if (!g_default)
        return -1;
    arena_set_trace_mode_r(g_default, g_pending_trace_mode);
    return 0;
}

ARENA_EXPORT
int arena_eval_base(const char *src)
{
    return arena_reactor_eval_base(g_default, src);
}

ARENA_EXPORT
void arena_freeze(void)
{
    arena_reactor_freeze(g_default);
}

ARENA_EXPORT
int arena_run(const char *src)
{
    return arena_run_r(g_default, src);
}

ARENA_EXPORT
int arena_run_module(const char *entry_name, const char *entry_src)
{
    return arena_run_module_r(g_default, entry_name, entry_src);
}

ARENA_EXPORT
void arena_set_trace_mode(int mode)
{
    g_pending_trace_mode = mode;
    arena_set_trace_mode_r(g_default, mode);
}

ARENA_EXPORT
void arena_set_request_mode(int mode)
{
    arena_set_request_mode_r(g_default, mode);
}

ARENA_EXPORT
void arena_set_random_seed(uint32_t seed_lo, uint32_t seed_hi)
{
    arena_set_random_seed_r(g_default,
                            ((uint64_t)seed_hi << 32) | seed_lo);
}

ARENA_EXPORT
void arena_set_date_now(uint32_t lo, uint32_t hi)
{
    arena_set_date_now_r(g_default,
                         (int64_t)(((uint64_t)hi << 32) | lo));
}

ARENA_EXPORT
int arena_snapshot_here(void)
{
    return arena_trace_snapshot_here();
}

ARENA_EXPORT int arena_oom_hit(void)
{
    return arena_oom_hit_r(g_default);
}
ARENA_EXPORT double arena_oom_requested(void)
{
    return (double)arena_oom_requested_r(g_default);
}
ARENA_EXPORT double arena_oom_used(void)
{
    return (double)arena_oom_used_r(g_default);
}
ARENA_EXPORT double arena_oom_limit(void)
{
    return (double)arena_oom_limit_r(g_default);
}

ARENA_EXPORT
void arena_destroy(void)
{
    arena_reactor_free(g_default);
    g_default = NULL;
}
