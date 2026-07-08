/*
 * arenajs trace emitter — fires structural events during JS execution so
 * the browser host can build a navigable timeline. See ARENA_PLAN.md
 * (or the design notes in qjs-arena-trace.c) for the why; this header
 * is just the public surface the patched QJS interpreter calls into.
 *
 * Modes:
 *   ARENA_TRACE_OFF   — every hook is a single nullable-check branch
 *   ARENA_TRACE_SCAN  — FUNC_ENTER / FUNC_EXIT / THROW events only
 *   ARENA_TRACE_DRILL — also LINE events on every source-line transition
 *
 * Stop semantics: the host's host_trace callback returns truthy to halt
 * execution. The C side raises a sentinel exception (message exactly
 * ARENA_TRACE_STOP_MSG) which arena_run_module recognises and converts
 * to a clean rc=0 return — the host knows it stopped on purpose.
 */
#ifndef QJS_ARENA_TRACE_H
#define QJS_ARENA_TRACE_H

#include <stdint.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARENA_TRACE_OFF   0
#define ARENA_TRACE_SCAN  1
#define ARENA_TRACE_DRILL 2

/* Compile-time gate. Default 0 — the trace machinery is invisible to
   the native worker build (zero code, zero per-opcode cost). The
   browser-targeted qjs_arena_wasm CMake target sets this to 1 to wire
   up the real implementation. */
#ifndef ARENA_TRACE_ENABLED
#define ARENA_TRACE_ENABLED 0
#endif

struct JSFunctionBytecode;

#if ARENA_TRACE_ENABLED

/* ── per-instance trace state ──────────────────────────────────────────
 * All mutable emitter state is per-instance, owned by the ArenaReactor
 * that created the runtime and bound to it via js_arena_trace_set_state
 * (a dedicated JSRuntime slot — NOT the user opaque, which belongs to
 * the embedder). Interpreter hooks resolve it from ctx → rt; a runtime
 * with no bound state (NULL slot) traces nothing. This is what makes
 * multiple reactors on one thread — including a run nested inside
 * another instance's run via a native callback — correct by
 * construction: each run only ever touches its own state.
 *
 * Fields are private to qjs-arena-trace.c except `mode`, which the
 * reactor reads/writes directly and the interpreter patch sites load
 * (see arena_trace_mode_of in quickjs.c).
 *
 * NOTE for hosts consuming events from more than one instance: JSAtom
 * ids are per-JSRuntime, and each instance interns NAME events
 * independently — scope your atom→string map per run (or per
 * instance), never globally across instances. */
#define ARENA_TRACE_NAME_CAP    1024
#define ARENA_TRACE_SCRATCH_CAP 4096

typedef struct ArenaTraceState {
    int mode;                    /* ARENA_TRACE_OFF/SCAN/DRILL */
    int bail_armed;              /* stop sentinel raised this run */
    /* live trace-event context, valid only while a host callback is
       dispatching (arena_trace_snapshot_here reads these) */
    JSContext *active_ctx;
    struct JSFunctionBytecode *active_b;
    const uint8_t *active_pc;
    /* per-run "last line emitted" tracker (DRILL dedup) */
    int      last_line_emitted;
    uint32_t last_file_atom;
    /* bounded NAME intern table — atoms already announced to the host */
    int      name_count;
    uint32_t name_table[ARENA_TRACE_NAME_CAP];
    /* payload assembly buffer; borrowed by the host only for the
       duration of the callback, per the host contract */
    uint8_t  scratch[ARENA_TRACE_SCRATCH_CAP];
} ArenaTraceState;

/* Bind/read the per-runtime state slot (defined in quickjs.c, which
   owns the JSRuntime layout). Binding is a base-memory write: do it
   pre-freeze, once, and never rewrite it. */
void js_arena_trace_set_state(JSRuntime *rt, struct ArenaTraceState *st);
struct ArenaTraceState *js_arena_trace_get_state(JSRuntime *rt);

/* Sentinel exception message used to signal a clean host-requested
   stop. arena_run_module compares the thrown error's message against
   this string and returns 0 if it matches. */
extern const char ARENA_TRACE_STOP_MSG[];

void arena_trace_state_init(ArenaTraceState *st);  /* fresh instance, mode OFF */
void arena_trace_reset(ArenaTraceState *st);       /* between runs; keeps mode */

#ifndef __EMSCRIPTEN__
/* ── native trace sink ─────────────────────────────────────────────────
 * In the browser build the emitter calls Module.host_trace / host_state
 * (see qjs-arena-trace.c). A native build has no JS host, so it dispatches
 * to a C callback the embedder registers here.
 *
 * on_event receives, for each structural event, the event `kind` and the
 * raw little-endian payload bytes — the SAME wire format the browser host
 * gets via Module.host_trace(kind, ptr, len):
 *
 *   kind 0  NAME       [atom u32][len u16][utf8 bytes]   atom→string intern
 *   kind 1  FUNC_ENTER [name_atom u32][file_atom u32][line u32]
 *   kind 2  FUNC_EXIT  (no payload)
 *   kind 3  LINE       [file_atom u32][line u32]         DRILL mode only
 *   kind 4  THROW      [file_atom u32][line u32][len u16][utf8 message]
 *
 * Atoms are interned: a NAME event is emitted once before the first event
 * that references a given atom, so the host can build an id→string table.
 *
 * Return value controls execution, mirroring the browser contract:
 *   0  continue
 *   1  stop  (engine raises the stop sentinel; arena_run* returns rc 0)
 *   2  stop AND inspect (engine walks the live stack, ships a JSON
 *      snapshot to on_state, then stops)
 * Any other value is treated as 0.
 *
 * on_state receives that JSON stack snapshot (UTF-8, not NUL-terminated —
 * use the length) when on_event returns 2, or on an explicit
 * arena_trace_snapshot_here() call. It may be NULL if the host never
 * returns 2. `user` is passed back to both callbacks unchanged.
 *
 * The payload pointer and snapshot bytes are owned by the engine and only
 * valid for the duration of the call — copy anything you need to keep. */
typedef int  (*arena_trace_event_fn)(int kind, const uint8_t *payload,
                                     int len, void *user);
typedef void (*arena_trace_state_fn)(const uint8_t *payload, int len,
                                     void *user);

void arena_trace_set_host(arena_trace_event_fn on_event,
                          arena_trace_state_fn on_state, void *user);
#endif

/* Walk the live stack and ship the same JSON snapshot host_trace=2
   emits, via _arena_host_state. Unlike host_trace=2, does NOT raise
   the stop sentinel — execution continues. Must be called
   synchronously from inside a host_trace callback. Returns 0 on
   success, -1 if called outside an active trace event. */
int arena_trace_snapshot_here(void);

/* True when raise_stop has been called this run — host_trace returned
   truthy and we tried to throw the sentinel. arena_run_module checks
   this so it can distinguish "stop sentinel was requested but its
   allocation OOM-d" (treat as clean stop) from "unrelated exception". */
int arena_trace_stop_armed(const ArenaTraceState *st);

void arena_trace_func_enter(JSContext *ctx, struct JSFunctionBytecode *b);
void arena_trace_func_exit(JSContext *ctx);
void arena_trace_check_line(JSContext *ctx,
                            struct JSFunctionBytecode *b,
                            const uint8_t *pc);
void arena_trace_op_throw(JSContext *ctx,
                          struct JSFunctionBytecode *b,
                          const uint8_t *pc,
                          JSValueConst thrown);

JSAtom         js_arena_trace_bc_filename(struct JSFunctionBytecode *b);
JSAtom         js_arena_trace_bc_func_name(struct JSFunctionBytecode *b);
int            js_arena_trace_bc_resolve_line(JSContext *ctx,
                                              struct JSFunctionBytecode *b,
                                              const uint8_t *pc);

/* Stack-walker accessors. The trace module calls these to enumerate
   live frames + locals when host_trace returns 2 (stop-and-inspect).
   JSStackFrame is forward-declared so the header doesn't have to
   include quickjs.c internals. */
struct JSStackFrame;

struct JSStackFrame *js_arena_trace_top_frame(JSContext *ctx);
struct JSStackFrame *js_arena_trace_prev_frame(struct JSStackFrame *sf);
struct JSFunctionBytecode *js_arena_trace_frame_bytecode(struct JSStackFrame *sf);
const uint8_t *js_arena_trace_frame_pc(struct JSStackFrame *sf);
/* Total names enumerable on this frame = arg_count + var_count.
   Args come first (indices 0..arg_count-1), then locals. */
int  js_arena_trace_frame_var_count(struct JSStackFrame *sf,
                                    struct JSFunctionBytecode *b);
JSAtom js_arena_trace_frame_var_name(struct JSStackFrame *sf,
                                     struct JSFunctionBytecode *b,
                                     int idx);
JSValueConst js_arena_trace_frame_var_value(struct JSStackFrame *sf,
                                            struct JSFunctionBytecode *b,
                                            int idx);

/* Closure variables this function references from enclosing scopes.
   For a module body the closure_var list typically contains the
   module's own top-level `let`/`const` names too (the module body is
   itself a function and inner scopes need to capture those). */
int  js_arena_trace_frame_closure_count(struct JSFunctionBytecode *b);
JSAtom js_arena_trace_frame_closure_name(struct JSFunctionBytecode *b, int idx);
JSValueConst js_arena_trace_frame_closure_value(struct JSStackFrame *sf,
                                                struct JSFunctionBytecode *b,
                                                int idx);

#else  /* !ARENA_TRACE_ENABLED — native worker build path */

/* Compile-time constant 0. Every `if (arena_trace_mode_of(ctx) !=
   ARENA_TRACE_OFF)` in quickjs.c folds to a constant-false branch the
   compiler removes at any optimization level, so the per-opcode
   dispatch hook is physically not emitted. (The trace-enabled variant
   of this macro lives in quickjs.c, next to the JSRuntime layout it
   reads.) */
#define arena_trace_mode_of(ctx) 0

/* Empty inline stubs so unfolded calls (if any) compile to no-ops
   without needing a linker symbol. Belt and braces — the macro above
   should keep these calls from being emitted in the first place. */
static inline void arena_trace_func_enter(JSContext *ctx, struct JSFunctionBytecode *b)
    { (void)ctx; (void)b; }
static inline void arena_trace_func_exit(JSContext *ctx) { (void)ctx; }
static inline void arena_trace_check_line(JSContext *ctx, struct JSFunctionBytecode *b,
                                          const uint8_t *pc)
    { (void)ctx; (void)b; (void)pc; }
static inline void arena_trace_op_throw(JSContext *ctx, struct JSFunctionBytecode *b,
                                        const uint8_t *pc, JSValueConst thrown)
    { (void)ctx; (void)b; (void)pc; (void)thrown; }

#endif

#ifdef __cplusplus
}
#endif

#endif
