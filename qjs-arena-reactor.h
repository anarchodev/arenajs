/*
 * arenajs reactor — the embedder-facing run loop over the dual-arena
 * runtime. Builds a frozen base (runtime + context + replay bindings +
 * module loader), then executes one-shot requests against it, resetting
 * the request region between runs.
 *
 * Two API layers, one implementation:
 *
 *   Instance API (ArenaReactor*) — native embedders (rove's replay /
 *   simulation drivers). Create as many reactors as you need.
 *
 *   Singleton exports (arena_init / arena_run / ...) — thin wrappers
 *   over one default instance. This is the frozen WASM ABI consumed by
 *   the browser replay shell (-sEXPORTED_FUNCTIONS) and the existing
 *   native CLI examples; signatures never change, and the u32 lo/hi
 *   splits + double returns exist purely so the wasm32 boundary needs
 *   no BigInt. New code should use the instance API.
 *
 * ── multi-instance contract ──────────────────────────────────────────
 *
 * Multiple reactors on ONE thread is a SUPPORTED configuration
 * (verified 2026-07-01: all arena state is per-runtime). The intended
 * shape is a long-lived harness runtime plus a resettable simulation
 * runtime side by side. Nothing in the reactor assumes process-global
 * or thread-local uniqueness:
 *
 *   - Runs may NEST: a native callback (tape responder, trace sink)
 *     fired during one instance's run may synchronously run modules on
 *     another instance. Each instance's trace state, determinism pins,
 *     OOM record, and entry-module slot are its own; a nested run
 *     cannot corrupt the outer one.
 *
 *   - Reset semantics are per-instance: arena_run*_r resets ONLY that
 *     instance's request region (at entry, so results/trace stay
 *     readable between runs). Other instances are untouched.
 *
 *   - What remains process-global, deliberately: the host callback
 *     registrations arena_trace_set_host() / arena_replay_set_host()
 *     are shared by all instances (one sink, one tape responder set).
 *     A host driving several instances multiplexes in its callbacks —
 *     it always knows which instance it is running, because runs are
 *     synchronous. In the browser build the same applies to the
 *     Module-level imports (Module.tapes, Module.host_trace, ...).
 *
 *   - NOT supported: concurrent runs on different THREADS, even with
 *     distinct instances (the host-callback registrations and the
 *     in-hook dispatch slot behind arena_snapshot_here are process
 *     scope). One thread at a time, N instances, nesting allowed.
 *
 *   - Trace NAME events intern JSAtom ids, which are per-runtime: a
 *     host consuming events from more than one instance must scope its
 *     atom→string map per run/instance, never globally.
 *
 * The reactor owns the JSRuntime it creates — including the runtime's
 * user opaque slot (JS_SetRuntimeOpaque), which it uses to find the
 * instance from a JSContext. Do not free an instance from inside any
 * run that is still on the C stack (its own or one it was invoked
 * from).
 */
#ifndef QJS_ARENA_REACTOR_H
#define QJS_ARENA_REACTOR_H

#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* arena_run / arena_run_module return codes. The caller must be able
   to tell "the JS code threw" — a user error, surface it, don't retry
   — from "we ran out of request arena" — a capacity signal: the
   result is void, resize / retry / 503, and tell the operator.

     ARENA_RC_OK     0   success, or clean host-requested stop
     ARENA_RC_ERROR -1   JS exception (user error)
     ARENA_RC_OOM   -2   request arena exhausted — result is void;
                         query arena_oom_* for actionable numbers */
#define ARENA_RC_OK     0
#define ARENA_RC_ERROR (-1)
#define ARENA_RC_OOM   (-2)

/* ── instance API ──────────────────────────────────────────────────── */

typedef struct ArenaReactor ArenaReactor;

/* Build a reactor: runtime + context, replay bindings + module loader
   installed pre-freeze, then freeze. Sizes are in KiB; <= 0 selects
   the 4 MiB default. Returns NULL on failure. */
ArenaReactor *arena_reactor_new(int base_kb, int request_kb);

/* Like arena_reactor_new but leaves the base UNFROZEN so the embedder can add
   more base globals (arena_reactor_eval_base) before sealing it with
   arena_reactor_freeze. Required call order: new_open → [eval_base…] → freeze
   → run. Returns NULL on failure. */
ArenaReactor *arena_reactor_new_open(int base_kb, int request_kb);

/* Eval `src` as a classic script into the UNFROZEN base (all allocations land
   in base pre-freeze, so the globals survive per-request resets). Pending jobs
   drain before returning. Returns ARENA_RC_*; ARENA_RC_ERROR if already frozen
   or on a JS exception. */
int arena_reactor_eval_base(ArenaReactor *r, const char *src);

/* Freeze the base — subsequent allocations are request-scoped. Idempotent.
   Required before the first run of an arena_reactor_new_open reactor
   (arena_reactor_new does it internally). */
void arena_reactor_freeze(ArenaReactor *r);

/* Tear down the instance. NULL is a no-op. See the header comment:
   never call this from inside a run. */
void arena_reactor_free(ArenaReactor *r);

/* Reset this instance's request arena, then eval `src` as a classic
   script, printing the completion value to stdout. Returns ARENA_RC_*. */
int arena_run_r(ArenaReactor *r, const char *src);

/* Reset this instance's request arena, then evaluate `entry_src` as
   the body of a module named `entry_name`; imports route through the
   replay module loader. Microtasks drain before returning. Returns
   ARENA_RC_*. */
int arena_run_module_r(ArenaReactor *r, const char *entry_name,
                       const char *entry_src);

/* Trace emitter mode for this instance (ARENA_TRACE_OFF/SCAN/DRILL,
   qjs-arena-trace.h). Takes effect immediately — hooks read the live
   value — and is sticky across runs. */
void arena_set_trace_mode_r(ArenaReactor *r, int mode);

/* Request-allocator regime for this instance's SUBSEQUENT runs
   (qjs-arena.h JSArenaReqMode: 0 = GC mspace, 1 = bump). Binds at the
   next request-arena reset — which arena_run*_r perform at entry — so
   call between runs, before the run it should govern. */
void arena_set_request_mode_r(ArenaReactor *r, int mode);

/* Determinism pins, per instance. Sticky until overwritten: every run
   re-applies the latest value after its request-arena reset. */
void arena_set_random_seed_r(ArenaReactor *r, uint64_t seed);
void arena_set_date_now_r(ArenaReactor *r, int64_t ms);

/* Per-instance OOM record of the most recent run (reset at each run's
   entry). _hit is 1 if any request-mode allocation was refused. */
int    arena_oom_hit_r(ArenaReactor *r);
size_t arena_oom_requested_r(ArenaReactor *r);
size_t arena_oom_used_r(ArenaReactor *r);
size_t arena_oom_limit_r(ArenaReactor *r);

/* ── singleton wrappers (frozen WASM/CLI ABI over a default instance) */

int  arena_init(int base_kb, int request_kb);   /* -1 if already live */
int  arena_run(const char *src);
int  arena_run_module(const char *entry_name, const char *entry_src);
void arena_set_trace_mode(int mode);
void arena_set_request_mode(int mode);
void arena_set_random_seed(uint32_t seed_lo, uint32_t seed_hi);
void arena_set_date_now(uint32_t lo, uint32_t hi);
int  arena_snapshot_here(void);   /* only from inside a trace callback */
int    arena_oom_hit(void);
double arena_oom_requested(void);
double arena_oom_used(void);
double arena_oom_limit(void);
void arena_destroy(void);

/* ── internal (replay bindings) ────────────────────────────────────── */

/* The JSModuleDef being evaluated by the arena_run_module call
   currently executing on ctx's runtime; NULL outside a module run.
   Consumed by __arena_entry_ns so replay epilogues can reach an
   anonymous `export default`. Resolved per instance via the runtime
   opaque, so nested runs on sibling instances each see their own. */
JSModuleDef *arena_entry_module(JSContext *ctx);

#ifdef __cplusplus
}
#endif

#endif
