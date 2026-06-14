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
#include <math.h>

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
   Anything else is treated as "continue".

   The payload is passed as a real pointer, NOT an `int` address. On
   wasm32 a pointer is 32 bits so emscripten marshals it to JS as the
   numeric address verbatim (Module.host_trace still receives a Number).
   On a 64-bit native build the same call hands the C host the genuine
   pointer — casting it through `int` at the call site (as the old wasm-
   only code did) would truncate the high half and hand the callback a
   garbage address. One signature, both targets. */
#ifdef __EMSCRIPTEN__

EM_JS(int, _arena_host_trace,
      (int kind, const uint8_t *payload, int payload_len), {
    if (!Module.host_trace) return 0;
    const r = Module.host_trace(kind, payload, payload_len);
    return (r === 1 || r === 2) ? r : 0;
});

/* host_state delivers a JSON-encoded snapshot of all live stack frames
   (each frame: function name, file, line, vars). Host parses with
   JSON.parse and renders. Fires only when host_trace returned 2. */
EM_JS(void, _arena_host_state,
      (const uint8_t *payload, int payload_len), {
    if (Module.host_state) Module.host_state(payload, payload_len);
});

#else  /* native: dispatch to a host-registered C callback */

static arena_trace_event_fn s_event_cb = NULL;
static arena_trace_state_fn s_state_cb = NULL;
static void               *s_cb_user   = NULL;

void arena_trace_set_host(arena_trace_event_fn on_event,
                          arena_trace_state_fn on_state, void *user)
{
    s_event_cb = on_event;
    s_state_cb = on_state;
    s_cb_user  = user;
}

/* Mirror the JS shim exactly: no sink installed behaves like a missing
   Module.host_trace (return 0 = continue), and any return other than
   1 or 2 is clamped to 0 so a careless host can't wedge execution. */
static int _arena_host_trace(int kind, const uint8_t *payload, int payload_len)
{
    if (!s_event_cb) return 0;
    int r = s_event_cb(kind, payload, payload_len, s_cb_user);
    return (r == 1 || r == 2) ? r : 0;
}

static void _arena_host_state(const uint8_t *payload, int payload_len)
{
    if (s_state_cb) s_state_cb(payload, payload_len, s_cb_user);
}

#endif

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
    return _arena_host_trace(0, scratch, (int)(6 + slen));
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
 * When host_trace returns 2 (or arena_trace_snapshot_here is called),
 * we synchronously walk the live stack and ship a JSON document:
 *   [ { "func":"name", "file":"path", "line":N,
 *       "vars": { "name1": <json>, ... } }, ... ]
 *
 * This is serialized DIRECTLY into a libc-malloc'd byte buffer — no
 * JSValue tree is built. The old implementation constructed a frames
 * array of objects-of-objects and handed it to JS_JSONStringify;
 * every one of those JSObject / JSShape / property-array / string
 * allocations landed in the request bump arena and never came back
 * until reset (bump frees are no-ops). Dense snapshot cadence × deep
 * stacks exhausted the arena and OOM-d the run. Writing bytes by hand
 * means the only arena allocations are the transient per-value
 * stringifications of genuinely-complex values (objects/arrays/large
 * floats), one at a time, each freed immediately — primitive locals
 * (the deep-recursion / tight-loop hot path) allocate nothing.
 *
 * The output buffer uses libc malloc/realloc/free, which DO reclaim
 * (emscripten dlmalloc), so it doesn't touch the bump arena at all.
 *
 * Value encoding preserves the old safe_for_json semantics: undefined,
 * uninitialized, symbol, function, and unserializable (cyclic/BigInt)
 * values become bracketed placeholder strings so the host sees that
 * something was there rather than a silently-dropped field.
 */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    err;   /* set on malloc failure → emit is skipped entirely */
} jbuf;

static void jb_grow(jbuf *j, size_t need)
{
    if (j->err || j->len + need <= j->cap) return;
    size_t ncap = j->cap ? j->cap : 4096;
    while (ncap < j->len + need) ncap *= 2;
    char *nb = (char *)realloc(j->buf, ncap);
    if (!nb) { j->err = 1; return; }
    j->buf = nb;
    j->cap = ncap;
}

static void jb_raw(jbuf *j, const char *s, size_t n)
{
    jb_grow(j, n);
    if (j->err) return;
    memcpy(j->buf + j->len, s, n);
    j->len += n;
}

static void jb_cstr(jbuf *j, const char *s) { jb_raw(j, s, strlen(s)); }

static void jb_ch(jbuf *j, char c)
{
    jb_grow(j, 1);
    if (!j->err) j->buf[j->len++] = c;
}

/* Append s as a quoted, RFC-8259-escaped JSON string. Raw UTF-8 bytes
   >= 0x20 pass through; only the seven short escapes + \uXXXX for
   other control chars. */
static void jb_json_str(jbuf *j, const char *s, size_t n)
{
    jb_ch(j, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  jb_raw(j, "\\\"", 2); break;
            case '\\': jb_raw(j, "\\\\", 2); break;
            case '\b': jb_raw(j, "\\b", 2);  break;
            case '\f': jb_raw(j, "\\f", 2);  break;
            case '\n': jb_raw(j, "\\n", 2);  break;
            case '\r': jb_raw(j, "\\r", 2);  break;
            case '\t': jb_raw(j, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char u[7];
                    snprintf(u, sizeof u, "\\u%04x", c);
                    jb_raw(j, u, 6);
                } else {
                    jb_ch(j, (char)c);
                }
        }
    }
    jb_ch(j, '"');
}

/* Resolve an atom to its string and append it as a JSON string.
   JS_AtomToCString allocates a transient cstring in the arena (freed
   immediately); the set of distinct names across a run is small and
   bounded, so this is not the allocation that mattered. */
static void jb_atom_str(JSContext *ctx, jbuf *j, JSAtom atom,
                        const char *fallback)
{
    const char *s = JS_AtomToCString(ctx, atom);
    if (s) {
        jb_json_str(j, s, strlen(s));
        JS_FreeCString(ctx, s);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));   /* clear conversion OOM */
        jb_json_str(j, fallback, strlen(fallback));
    }
}

/* Append one variable value as JSON. Primitives format directly with
   zero JS allocation. Strings/functions cost one transient cstring.
   Objects/arrays defer to JS_JSONStringify for that single value —
   the only place a JSValue temporary is created, and it's freed
   before the next value. Mirrors the old safe_for_json placeholders. */
static void jb_value(JSContext *ctx, jbuf *j, JSValueConst v)
{
    int tag = JS_VALUE_GET_NORM_TAG(v);

    if (JS_IsUndefined(v))           { jb_cstr(j, "\"[undefined]\"");      return; }
    if (tag == JS_TAG_UNINITIALIZED) { jb_cstr(j, "\"[uninitialized]\"");  return; }
    if (JS_IsNull(v))                { jb_cstr(j, "null");                 return; }
    if (JS_IsBool(v)) { jb_cstr(j, JS_ToBool(ctx, v) ? "true" : "false");  return; }
    if (tag == JS_TAG_SYMBOL)        { jb_cstr(j, "\"[Symbol]\"");         return; }

    if (tag == JS_TAG_INT) {
        char tmp[16];
        int n = snprintf(tmp, sizeof tmp, "%d", JS_VALUE_GET_INT(v));
        jb_raw(j, tmp, (size_t)n);
        return;
    }
    if (tag == JS_TAG_FLOAT64) {
        double d = JS_VALUE_GET_FLOAT64(v);
        if (!isfinite(d)) { jb_cstr(j, "null"); return; }  /* JSON has no NaN/Inf */
        if (d == (double)(int64_t)d && d >= -9.007199254740992e15
                                    && d <=  9.007199254740992e15) {
            char tmp[24];
            int n = snprintf(tmp, sizeof tmp, "%lld", (long long)(int64_t)d);
            jb_raw(j, tmp, (size_t)n);
            return;
        }
        /* fractional / large: engine's number formatter (small temp) */
        JSValue s = JS_ToString(ctx, v);
        if (!JS_IsException(s)) {
            size_t n; const char *cs = JS_ToCStringLen(ctx, &n, s);
            if (cs) { jb_raw(j, cs, n); JS_FreeCString(ctx, cs); }
            else { JS_FreeValue(ctx, JS_GetException(ctx)); jb_cstr(j, "null"); }
        } else { JS_FreeValue(ctx, JS_GetException(ctx)); jb_cstr(j, "null"); }
        JS_FreeValue(ctx, s);
        return;
    }

    if (JS_IsString(v)) {
        size_t n; const char *s = JS_ToCStringLen(ctx, &n, v);
        if (s) { jb_json_str(j, s, n); JS_FreeCString(ctx, s); }
        else { JS_FreeValue(ctx, JS_GetException(ctx));
               jb_cstr(j, "\"[unserializable]\""); }
        return;
    }

    if (JS_IsFunction(ctx, v)) {
        JSValue nm = JS_GetPropertyStr(ctx, v, "name");
        const char *n = NULL;
        if (!JS_IsException(nm) && !JS_IsUndefined(nm))
            n = JS_ToCString(ctx, nm);
        char buf[96];
        snprintf(buf, sizeof buf, "[function %s]", n ? n : "");
        if (n) JS_FreeCString(ctx, n);
        else JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, nm);
        jb_json_str(j, buf, strlen(buf));
        return;
    }

    /* object / array / anything else: stringify just this one value */
    JSValue js = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(js)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        jb_cstr(j, "\"[unserializable]\"");
    } else if (JS_IsUndefined(js)) {
        jb_cstr(j, "\"[unserializable]\"");
    } else {
        size_t n; const char *s = JS_ToCStringLen(ctx, &n, js);
        if (s) { jb_raw(j, s, n); JS_FreeCString(ctx, s); }
        else { JS_FreeValue(ctx, JS_GetException(ctx));
               jb_cstr(j, "\"[unserializable]\""); }
    }
    JS_FreeValue(ctx, js);
}

static void emit_state(JSContext *ctx,
                       struct JSFunctionBytecode *top_b,
                       const uint8_t *top_pc)
{
    if (s_bail_armed) return;

    jbuf j = { 0 };
    jb_ch(&j, '[');

    struct JSStackFrame *sf = js_arena_trace_top_frame(ctx);
    int is_top = 1;
    int first_frame = 1;

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

        if (!first_frame) jb_ch(&j, ',');
        first_frame = 0;

        jb_cstr(&j, "{\"func\":");
        jb_atom_str(ctx, &j, js_arena_trace_bc_func_name(b), "<anonymous>");
        jb_cstr(&j, ",\"file\":");
        jb_atom_str(ctx, &j, js_arena_trace_bc_filename(b), "");
        {
            char ln[40];
            int n = snprintf(ln, sizeof ln, ",\"line\":%d,\"vars\":{", line);
            jb_raw(&j, ln, (size_t)n);
        }

        /* vars: args + locals first (they win on name collision),
           then closure-referenced names. Track emitted local names so
           a colliding closure var doesn't write a duplicate JSON key
           (preserves the old "locals win" dedup). The 64-entry cap is
           a pragmatic bound — a function with >64 locals colliding
           with a closure name beyond the cap would emit a duplicate
           key, which JSON.parse tolerates (last wins); vanishingly
           rare and non-fatal. */
        int first_var = 1;
        JSAtom seen[64];
        int seen_n = 0;
        int n = js_arena_trace_frame_var_count(sf, b);
        for (int i = 0; i < n; i++) {
            JSAtom na = js_arena_trace_frame_var_name(sf, b, i);
            if (na == JS_ATOM_NULL) continue;
            if (!first_var) jb_ch(&j, ',');
            first_var = 0;
            jb_atom_str(ctx, &j, na, "");
            jb_ch(&j, ':');
            jb_value(ctx, &j, js_arena_trace_frame_var_value(sf, b, i));
            if (seen_n < (int)(sizeof seen / sizeof seen[0]))
                seen[seen_n++] = na;
        }
        int cn = js_arena_trace_frame_closure_count(b);
        for (int i = 0; i < cn; i++) {
            JSAtom na = js_arena_trace_frame_closure_name(b, i);
            if (na == JS_ATOM_NULL) continue;
            int dup = 0;
            for (int k = 0; k < seen_n; k++)
                if (seen[k] == na) { dup = 1; break; }
            if (dup) continue;
            if (!first_var) jb_ch(&j, ',');
            first_var = 0;
            jb_atom_str(ctx, &j, na, "");
            jb_ch(&j, ':');
            jb_value(ctx, &j, js_arena_trace_frame_closure_value(sf, b, i));
        }

        jb_cstr(&j, "}}");
        sf = js_arena_trace_prev_frame(sf);
    }

    jb_ch(&j, ']');

    if (!j.err)
        _arena_host_state((const uint8_t *)j.buf, (int)j.len);
    free(j.buf);

    /* emit_state must NEVER leave a pending exception on the context.
       The per-value paths above clear their own conversion failures,
       but be defensive: snapshot_here is a fire-and-forget path from
       the host's perspective. JS_GetException returns JS_NULL (a
       no-op for JS_FreeValue) when nothing is pending, so this is
       safe to call unconditionally. */
    JSValue stale = JS_GetException(ctx);
    JS_FreeValue(ctx, stale);
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

int arena_trace_stop_armed(void)
{
    return s_bail_armed;
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
    stop = merge_stop(stop, _arena_host_trace(1, scratch, 12));
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
    int stop = _arena_host_trace(2, scratch, 0);
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
    stop = merge_stop(stop, _arena_host_trace(3, scratch, 8));
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
    stop = merge_stop(stop, _arena_host_trace(4, scratch, (int)(10 + mlen)));
    s_active_ctx = NULL; s_active_b = NULL; s_active_pc = NULL;
    handle_stop_code(ctx, stop, b, pc);
}

#endif /* ARENA_TRACE_ENABLED */
