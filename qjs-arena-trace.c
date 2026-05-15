/*
 * arenajs trace emitter implementation.
 *
 * Binary wire format (all multi-byte fields little-endian):
 *
 *   NAME       (kind=0) — emitted once per JSAtom that subsequent events
 *                         reference. Lets the host build a stable
 *                         id→string map without paying per-event string
 *                         cost.
 *     [u32 atom][u16 name_len][name_bytes ...]
 *
 *   FUNC_ENTER (kind=1)
 *     [u32 name_atom][u32 file_atom][u32 line]
 *
 *   FUNC_EXIT  (kind=2)
 *     (empty payload — host pairs with last unmatched FUNC_ENTER by stack)
 *
 *   LINE       (kind=3) — emitted only when the source line changes
 *     [u32 file_atom][u32 line]
 *
 *   THROW      (kind=4)
 *     [u32 file_atom][u32 line][u16 msg_len][msg_bytes ...]
 *
 * The atom values are JSAtoms — stable u32 ids within the runtime. We
 * pass them through as-is; the host doesn't need to interpret atoms,
 * just dedupe by their numeric value and resolve via NAME events.
 *
 * Name-table state lives in this module and is reset between runs by
 * the reactor's arena_run_module entry point. The host's mirror table
 * should be reset in lockstep — it can do that simply by emptying its
 * map when it sees the cursor reset (or just key by atom and accept
 * duplicate NAME emits across runs, which is harmless).
 */
#include "qjs-arena-trace.h"

#if ARENA_TRACE_ENABLED

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EM_JS(ret, name, args, body) static ret name args { return (ret)0; }
#endif

int arena_trace_mode = ARENA_TRACE_OFF;
const char ARENA_TRACE_STOP_MSG[] = "_arena_trace_stop_";

/* Bounded name-intern table. 1024 entries is overkill for any 10 ms
   handler — typical traces touch <100 distinct names. */
#define NAME_TABLE_CAP 1024
static uint32_t name_table[NAME_TABLE_CAP];
static int      name_table_count = 0;

/* Per-run "last line emitted" tracker. Reset on FUNC_ENTER/EXIT so
   line transitions across call boundaries always emit (the FUNC_*
   event itself carries the line of the boundary). */
static int  last_line_emitted = -1;
static uint32_t last_file_atom = 0;

/* Once a host_trace callback returns truthy, we set this flag and
   raise the stop-sentinel exception. All subsequent emits become
   no-ops so the host doesn't see a flurry of FUNC_EXIT events as the
   stack unwinds — clean "stopped at event N" semantics. */
static int s_bail_armed = 0;

/* Live trace-event context, stashed by each hook just before invoking
   _arena_host_trace and cleared on return. arena_trace_snapshot_here()
   reads these so a JS host_trace callback can synchronously request a
   stack snapshot without raising the stop sentinel. */
static JSContext *s_active_ctx = NULL;
static struct JSFunctionBytecode *s_active_b = NULL;
static const uint8_t *s_active_pc = NULL;

/* Scratch buffer for binary payloads. Static because emit is sync and
   single-threaded — the host copies bytes before our next emit. */
#define SCRATCH_CAP 4096
static uint8_t scratch[SCRATCH_CAP];

/* ── host import ───────────────────────────────────────────────────── */

/* host_trace return codes:
     0 — continue
     1 — stop (raise sentinel exception, unwind)
     2 — stop AND inspect (walk stack first, ship via host_state,
         then stop)
   Anything else is treated as "continue". */
EM_JS(int, _arena_host_trace,
      (int kind, int payload_ptr, int payload_len), {
    if (!Module.host_trace) return 0;
    const r = Module.host_trace(kind, payload_ptr, payload_len);
    return (r === 1 || r === 2) ? r : 0;
});

/* host_state delivers a JSON-encoded snapshot of all live stack frames
   (each frame: function name, file, line, vars). Host parses with
   JSON.parse and renders. Fires only when host_trace returned 2. */
EM_JS(void, _arena_host_state,
      (int payload_ptr, int payload_len), {
    if (Module.host_state) Module.host_state(payload_ptr, payload_len);
});

/* ── name-table interning ──────────────────────────────────────────── */

static bool name_seen(uint32_t atom)
{
    for (int i = 0; i < name_table_count; i++)
        if (name_table[i] == atom) return true;
    return false;
}

static void name_remember(uint32_t atom)
{
    if (name_table_count >= NAME_TABLE_CAP) return;  /* silently drop */
    name_table[name_table_count++] = atom;
}

/* Encode + emit a NAME event for a given atom. Looks up the atom's
   string form via JS_AtomToCString. Caller must have checked name_seen
   first. */
static int emit_name(JSContext *ctx, uint32_t atom)
{
    name_remember(atom);
    const char *s = JS_AtomToCString(ctx, (JSAtom)atom);
    if (!s) return 0;  /* atom doesn't resolve — skip, host will see id with no NAME */
    size_t slen = strlen(s);
    if (slen > 0xffff) slen = 0xffff;     /* clamp; truncated name is fine */
    if (slen + 6 > SCRATCH_CAP) slen = SCRATCH_CAP - 6;
    memcpy(scratch + 0, &atom, 4);
    uint16_t slen16 = (uint16_t)slen;
    memcpy(scratch + 4, &slen16, 2);
    memcpy(scratch + 6, s, slen);
    JS_FreeCString(ctx, s);
    return _arena_host_trace(0, (int)(intptr_t)scratch, (int)(6 + slen));
}

/* Guarantee NAME has been emitted for this atom before emitting an
   event that references it. Returns nonzero if the host signalled
   stop. */
static int ensure_name(JSContext *ctx, uint32_t atom)
{
    if (name_seen(atom)) return 0;
    return emit_name(ctx, atom);
}

/* ── stack-state emit (kind=2 inspection) ──────────────────────────── */
/*
 * When host_trace returns 2, we synchronously walk the live stack
 * before unwinding. Each frame becomes an object with:
 *   { func: "name", file: "path", line: N,
 *     vars: { name1: <jsonValue>, name2: <jsonValue>, ... } }
 *
 * The frames-array is built as a JSValue tree so we can lean on
 * QJS's JS_JSONStringify for the heavy lifting (handles strings,
 * numbers, nested objects, arrays). Values that JSON can't represent
 * (functions, symbols, undefined, cycles, BigInt) get coerced to
 * placeholder strings via safe_for_json so the host still SEES that
 * something was there rather than getting silently-dropped fields.
 */

static JSValue safe_for_json(JSContext *ctx, JSValueConst val)
{
    if (JS_IsUndefined(val))
        return JS_NewString(ctx, "[undefined]");
    if (JS_IsFunction(ctx, val)) {
        JSValue name = JS_GetPropertyStr(ctx, val, "name");
        const char *n = NULL;
        if (!JS_IsException(name) && !JS_IsUndefined(name))
            n = JS_ToCString(ctx, name);
        char buf[96];
        snprintf(buf, sizeof(buf), "[function %s]", n ? n : "");
        if (n) JS_FreeCString(ctx, n);
        JS_FreeValue(ctx, name);
        return JS_NewString(ctx, buf);
    }
    if (JS_VALUE_GET_TAG(val) == JS_TAG_SYMBOL)
        return JS_NewString(ctx, "[Symbol]");
    if (JS_VALUE_GET_TAG(val) == JS_TAG_UNINITIALIZED)
        return JS_NewString(ctx, "[uninitialized]");

    /* Probe-stringify to catch BigInt / cycles. If it throws we fall
       back to a placeholder; otherwise the outer JSON pass will
       re-encode the original value cleanly. */
    JSValue probe = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(probe)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NewString(ctx, "[unserializable]");
    }
    JS_FreeValue(ctx, probe);
    return JS_DupValue(ctx, val);
}

static void emit_state(JSContext *ctx,
                       struct JSFunctionBytecode *top_b,
                       const uint8_t *top_pc)
{
    if (s_bail_armed) return;

    JSValue frames_arr = JS_NewArray(ctx);
    if (JS_IsException(frames_arr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }

    struct JSStackFrame *sf = js_arena_trace_top_frame(ctx);
    int is_top = 1;
    uint32_t frame_idx = 0;

    while (sf) {
        struct JSFunctionBytecode *b = is_top ? top_b
                                              : js_arena_trace_frame_bytecode(sf);
        is_top = 0;
        if (!b) {
            /* C function or detached frame — skip. The chain may still
               continue past it with JS frames below. */
            sf = js_arena_trace_prev_frame(sf);
            continue;
        }
        const uint8_t *pc = top_pc ? top_pc : js_arena_trace_frame_pc(sf);
        top_pc = NULL;   /* only the topmost frame uses the live pc */
        int line = js_arena_trace_bc_resolve_line(ctx, b, pc);
        if (line < 0) line = 0;

        JSValue frame = JS_NewObject(ctx);

        /* func name */
        JSAtom fn_atom = js_arena_trace_bc_func_name(b);
        const char *fn = JS_AtomToCString(ctx, fn_atom);
        JS_SetPropertyStr(ctx, frame, "func",
                           JS_NewString(ctx, fn ? fn : "<anonymous>"));
        if (fn) JS_FreeCString(ctx, fn);

        /* file */
        JSAtom file_atom = js_arena_trace_bc_filename(b);
        const char *fname = JS_AtomToCString(ctx, file_atom);
        JS_SetPropertyStr(ctx, frame, "file",
                           JS_NewString(ctx, fname ? fname : ""));
        if (fname) JS_FreeCString(ctx, fname);

        /* line */
        JS_SetPropertyStr(ctx, frame, "line", JS_NewInt32(ctx, line));

        /* vars: args + locals + closure-referenced names. The walker
           handles each storage class (arg_buf, var_buf, captured-via-
           var_refs, closure-via-var_refs) under the hood; we just
           merge everything into a single object keyed by name. If the
           same name appears as both a local and a closure ref —
           rare in well-formed code — the local wins because it's
           visited first. */
        JSValue vars = JS_NewObject(ctx);
        int n = js_arena_trace_frame_var_count(sf, b);
        for (int i = 0; i < n; i++) {
            JSAtom name_atom = js_arena_trace_frame_var_name(sf, b, i);
            if (name_atom == JS_ATOM_NULL) continue;
            JSValueConst raw = js_arena_trace_frame_var_value(sf, b, i);
            JSValue safe = safe_for_json(ctx, raw);
            JS_SetProperty(ctx, vars, name_atom, safe);
        }
        int cn = js_arena_trace_frame_closure_count(b);
        for (int i = 0; i < cn; i++) {
            JSAtom name_atom = js_arena_trace_frame_closure_name(b, i);
            if (name_atom == JS_ATOM_NULL) continue;
            /* skip if already set as a local (locals win) */
            if (JS_HasProperty(ctx, vars, name_atom) == 1) continue;
            JSValueConst raw = js_arena_trace_frame_closure_value(sf, b, i);
            JSValue safe = safe_for_json(ctx, raw);
            JS_SetProperty(ctx, vars, name_atom, safe);
        }
        JS_SetPropertyStr(ctx, frame, "vars", vars);

        JS_SetPropertyUint32(ctx, frames_arr, frame_idx++, frame);
        sf = js_arena_trace_prev_frame(sf);
    }

    JSValue json = JS_JSONStringify(ctx, frames_arr, JS_UNDEFINED, JS_UNDEFINED);
    if (!JS_IsException(json)) {
        size_t slen = 0;
        const char *s = JS_ToCStringLen(ctx, &slen, json);
        if (s) {
            _arena_host_state((int)(intptr_t)s, (int)slen);
            JS_FreeCString(ctx, s);
        }
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, frames_arr);
}

/* ── stop handling ─────────────────────────────────────────────────── */

static void raise_stop(JSContext *ctx)
{
    if (s_bail_armed) return;
    JS_ThrowInternalError(ctx, "%s", ARENA_TRACE_STOP_MSG);
    s_bail_armed = 1;
}

/* Dispatch on host_trace return code. When the host returns 2, we
   emit the stack state before raising the stop sentinel so the host
   has the snapshot in hand by the time arena_run_module returns. */
static void handle_stop_code(JSContext *ctx, int code,
                             struct JSFunctionBytecode *b,
                             const uint8_t *pc)
{
    if (code == 2) emit_state(ctx, b, pc);
    if (code) raise_stop(ctx);
}

/* ── lifecycle ─────────────────────────────────────────────────────── */

void arena_trace_set_mode(int mode)
{
    arena_trace_mode = mode;
}

void arena_trace_reset(void)
{
    name_table_count = 0;
    last_line_emitted = -1;
    last_file_atom = 0;
    s_bail_armed = 0;
    s_active_ctx = NULL;
    s_active_b = NULL;
    s_active_pc = NULL;
}

int arena_trace_snapshot_here(void)
{
    if (!s_active_ctx || s_bail_armed) return -1;
    emit_state(s_active_ctx, s_active_b, s_active_pc);
    return 0;
}

/* ── hook implementations ──────────────────────────────────────────── */

/* Helper: combine two host_trace return codes. If either signals 2
   (stop+inspect), the merged code is 2; otherwise if either signals
   1, merged is 1; else 0. The "highest non-zero wins" rule lets
   intern-emitted NAME events promote the eventual main-event stop. */
static int merge_stop(int a, int b)
{
    if (a == 2 || b == 2) return 2;
    if (a || b) return 1;
    return 0;
}

void arena_trace_func_enter(JSContext *ctx, struct JSFunctionBytecode *b)
{
    if (arena_trace_mode == ARENA_TRACE_OFF || s_bail_armed) return;
    uint32_t name_atom = (uint32_t)js_arena_trace_bc_func_name(b);
    uint32_t file_atom = (uint32_t)js_arena_trace_bc_filename(b);
    int line = js_arena_trace_bc_resolve_line(ctx, b, NULL);
    if (line < 0) line = 0;

    s_active_ctx = ctx; s_active_b = b; s_active_pc = NULL;
    int stop = 0;
    stop = merge_stop(stop, ensure_name(ctx, name_atom));
    stop = merge_stop(stop, ensure_name(ctx, file_atom));

    memcpy(scratch + 0, &name_atom, 4);
    memcpy(scratch + 4, &file_atom, 4);
    uint32_t line32 = (uint32_t)line;
    memcpy(scratch + 8, &line32, 4);
    stop = merge_stop(stop, _arena_host_trace(1, (int)(intptr_t)scratch, 12));
    s_active_ctx = NULL; s_active_b = NULL; s_active_pc = NULL;

    /* New frame — reset line tracker so first LINE event in this frame
       (or first event after the matching exit) fires unconditionally. */
    last_line_emitted = -1;
    last_file_atom = 0;

    handle_stop_code(ctx, stop, b, NULL);
}

void arena_trace_func_exit(JSContext *ctx)
{
    if (arena_trace_mode == ARENA_TRACE_OFF || s_bail_armed) return;
    s_active_ctx = ctx; s_active_b = NULL; s_active_pc = NULL;
    int stop = _arena_host_trace(2, (int)(intptr_t)scratch, 0);
    s_active_ctx = NULL;
    last_line_emitted = -1;
    last_file_atom = 0;
    handle_stop_code(ctx, stop, NULL, NULL);
}

void arena_trace_check_line(JSContext *ctx,
                            struct JSFunctionBytecode *b,
                            const uint8_t *pc)
{
    if (arena_trace_mode < ARENA_TRACE_DRILL || s_bail_armed) return;
    int line = js_arena_trace_bc_resolve_line(ctx, b, pc);
    if (line < 0) return;
    uint32_t file_atom = (uint32_t)js_arena_trace_bc_filename(b);
    if ((uint32_t)line == (uint32_t)last_line_emitted && file_atom == last_file_atom)
        return;
    last_line_emitted = line;
    last_file_atom = file_atom;

    s_active_ctx = ctx; s_active_b = b; s_active_pc = pc;
    int stop = ensure_name(ctx, file_atom);
    memcpy(scratch + 0, &file_atom, 4);
    uint32_t line32 = (uint32_t)line;
    memcpy(scratch + 4, &line32, 4);
    stop = merge_stop(stop, _arena_host_trace(3, (int)(intptr_t)scratch, 8));
    s_active_ctx = NULL; s_active_b = NULL; s_active_pc = NULL;
    handle_stop_code(ctx, stop, b, pc);
}

void arena_trace_op_throw(JSContext *ctx,
                          struct JSFunctionBytecode *b,
                          const uint8_t *pc,
                          JSValueConst thrown)
{
    if (arena_trace_mode == ARENA_TRACE_OFF || s_bail_armed) return;
    int line = js_arena_trace_bc_resolve_line(ctx, b, pc);
    if (line < 0) line = 0;
    uint32_t file_atom = (uint32_t)js_arena_trace_bc_filename(b);
    s_active_ctx = ctx; s_active_b = b; s_active_pc = pc;
    int stop = ensure_name(ctx, file_atom);

    /* Best-effort message via toString. If it throws or returns NULL,
       emit empty message — host can still anchor to file:line. */
    const char *msg = JS_ToCString(ctx, thrown);
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > 0xffff) mlen = 0xffff;
    if (mlen + 10 > SCRATCH_CAP) mlen = SCRATCH_CAP - 10;

    memcpy(scratch + 0, &file_atom, 4);
    uint32_t line32 = (uint32_t)line;
    memcpy(scratch + 4, &line32, 4);
    uint16_t mlen16 = (uint16_t)mlen;
    memcpy(scratch + 8, &mlen16, 2);
    if (msg) memcpy(scratch + 10, msg, mlen);
    if (msg) JS_FreeCString(ctx, msg);
    stop = merge_stop(stop, _arena_host_trace(4, (int)(intptr_t)scratch, (int)(10 + mlen)));
    s_active_ctx = NULL; s_active_b = NULL; s_active_pc = NULL;
    handle_stop_code(ctx, stop, b, pc);
}

#endif /* ARENA_TRACE_ENABLED */
