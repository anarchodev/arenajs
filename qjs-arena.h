/*
 * QuickJS dual-region allocator: bump base, reclaiming request heap
 *
 * Two-region model for request-scoped JS execution:
 *   - base region: bump arena holding the snapshot (runtime, prelude,
 *     prototypes); built pre-freeze, never written again, never reset.
 *   - request region: a dlmalloc mspace confined to a fixed buffer.
 *     js_free actually reclaims, so refcount-zero objects return their
 *     memory mid-request (hybrid-gc branch; the master branch bump
 *     allocator's ceiling was cumulative allocation, this one's is peak
 *     live set).
 *
 * Each region owns a single contiguous buffer of fixed capacity, sized at
 * js_dual_arena_new() time. Allocations beyond capacity return NULL (which
 * propagates as JS OOM); neither buffer ever grows.
 *
 * The active region is selected by a mode flag flipped via
 * js_dual_arena_freeze(), which also creates the mspace over the request
 * buffer. Reset stays O(1): stomp a fresh mspace header over the same
 * (dirty) buffer — all allocator state lives inside it, so every prior
 * allocation is forgotten in constant time and pages stay resident.
 */
#ifndef QUICKJS_ARENA_H
#define QUICKJS_ARENA_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JSDualArena JSDualArena;

/* Fixed capacities for the two arenas. Pass 0 for a 16 MiB default. */
JS_EXTERN JSDualArena *js_dual_arena_new(size_t base_size,
                                         size_t request_size);
JS_EXTERN void js_dual_arena_free(JSDualArena *da);

JS_EXTERN void js_dual_arena_freeze(JSDualArena *da);
JS_EXTERN void js_dual_arena_reset_request(JSDualArena *da);
JS_EXTERN bool js_dual_arena_is_frozen(const JSDualArena *da);

JS_EXTERN bool js_dual_arena_in_base(const JSDualArena *da, const void *ptr);
JS_EXTERN bool js_dual_arena_in_request(const JSDualArena *da, const void *ptr);

JS_EXTERN size_t js_dual_arena_base_used(const JSDualArena *da);
JS_EXTERN size_t js_dual_arena_request_used(const JSDualArena *da);

/* Request-arena exhaustion record. The bump allocator is the only
 * component that knows for certain we hit the ceiling — by the time
 * the resulting OOM propagates, QJS may have mangled it into a bare
 * `null` exception (it can't allocate the Error object). So we record
 * the FIRST refused request-mode allocation at the source. Cleared by
 * js_dual_arena_reset_request, so it is strictly per-request.
 *
 * Embedders use this to distinguish "the JS code threw" (user error)
 * from "we ran out of arena" (capacity — resize / retry / 503) and to
 * surface an actionable message with real numbers.
 *
 * REQUEST ARENA ONLY. Pre-freeze (base-mode) refusals are deliberately
 * NOT recorded: exhausting base is a build-the-snapshot problem, not a
 * per-request capacity signal, and there is no reset to clear the latch
 * against. So this flag stays false no matter how far a snapshot
 * overruns its base arena — do not use it as a base-exhaustion guard.
 * Base exhaustion surfaces the ordinary way, as a thrown exception from
 * the JS_Eval that ran out, so checking JS_IsException on every
 * snapshot-time eval IS the base-exhaustion check. js_dual_arena_base_used
 * against the base size you passed to JS_NewRuntimeArena is the
 * headroom metric. */
JS_EXTERN bool js_dual_arena_oom_hit(const JSDualArena *da);
/* Bytes the refused allocation asked for (0 if no OOM this request). */
JS_EXTERN size_t js_dual_arena_oom_requested(const JSDualArena *da);
/* Request-arena bytes in use at the moment of refusal. */
JS_EXTERN size_t js_dual_arena_oom_used(const JSDualArena *da);
/* Request-arena total capacity. */
JS_EXTERN size_t js_dual_arena_oom_limit(const JSDualArena *da);

/* Thread-local list of registered base-arena address ranges. One entry
 * per arena-backed runtime that has been frozen on this thread. Used by
 * the refcount chokepoints (and a few other per-pointer checks) to
 * recognise base-arena allocations.
 *
 * Multiple arena runtimes can coexist in the same thread, and arena
 * runtimes can coexist with vanilla (non-arena) runtimes. Vanilla
 * heap pointers fall in no registered range, so js_arena_ptr_is_base()
 * returns false for them automatically.
 *
 * The runtime must stay on its creating thread — a second thread calling
 * into it sees an unset TLS list and js_arena_ptr_is_base() reports
 * false for objects that ARE in base, which corrupts refcount semantics.
 */
#define JS_ARENA_RANGES_MAX 16
struct js_arena_range { const uint8_t *lo, *hi; };
/* JS_EXTERN so a shared build exports them: js_arena_ptr_is_base() below is
   a static inline compiled into every consumer (the arena-* harnesses among
   them), so each one references these TLS symbols directly. Without the
   visibility attribute -fvisibility=hidden keeps them internal to libqjs and
   linking any consumer against a shared build fails. */
extern JS_EXTERN __thread struct js_arena_range js_arena_ranges[JS_ARENA_RANGES_MAX];
extern JS_EXTERN __thread int                   js_arena_range_count;

static inline bool js_arena_ptr_is_base(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    for (int i = 0; i < js_arena_range_count; i++) {
        if (b >= js_arena_ranges[i].lo && b < js_arena_ranges[i].hi)
            return true;
    }
    return false;
}

/* Internal: register/deregister an arena's base range. Called by
 * js_dual_arena_freeze and js_dual_arena_free. Returns 0 on success,
 * -1 if the per-thread range table is full (raise JS_ARENA_RANGES_MAX). */
JS_EXTERN int  js_arena_register_base(const uint8_t *lo, const uint8_t *hi);
JS_EXTERN void js_arena_unregister_base(const uint8_t *lo, const uint8_t *hi);

/* Same registry pattern for request-region ranges. Needed because
 * js_malloc_usable_size receives only the pointer — no opaque — and
 * must dispatch between mspace chunks (mspace_usable_size) and
 * bump-header allocations. Each entry carries a pointer to its arena's
 * CURRENT request mode: with per-reset allocator selection, whether a
 * request-range pointer is an mspace chunk or a bump allocation is a
 * property of the arena's mode this request — and every live request
 * pointer matches the current mode, because a reset (the only moment
 * the mode can change) kills all request allocations wholesale. */
struct js_arena_req_range {
    const uint8_t *lo, *hi;
    const uint8_t *mode;   /* -> JSDualArena.req_mode (JSArenaReqMode) */
};
extern __thread struct js_arena_req_range js_arena_req_ranges[JS_ARENA_RANGES_MAX];
extern __thread int                       js_arena_req_range_count;

static inline bool js_arena_ptr_is_request(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    for (int i = 0; i < js_arena_req_range_count; i++) {
        if (b >= js_arena_req_ranges[i].lo && b < js_arena_req_ranges[i].hi)
            return true;
    }
    return false;
}

JS_EXTERN int  js_arena_register_request(const uint8_t *lo, const uint8_t *hi,
                                         const uint8_t *mode);
JS_EXTERN void js_arena_unregister_request(const uint8_t *lo, const uint8_t *hi);

/* Fixed per-request state slot. The first JS_ARENA_REQUEST_SLOT_SIZE
 * bytes of the request region (after the 16-byte cursor prefix) are
 * reserved OUTSIDE the allocator's territory: the bump cursor's floor
 * sits past them and resets never reclaim them. JSRequestState lives
 * here at an address that is fixed for the life of the arena — so
 * rt->req is written exactly once (at freeze), resets re-initialize
 * the slot in place, and the address no longer depends on the request
 * allocator's behavior at all. That decoupling is what allows the
 * request-side allocator to change (or, later, be chosen per reset)
 * without a base write to re-point rt->req. */
#define JS_ARENA_REQUEST_SLOT_SIZE 512
JS_EXTERN void *js_dual_arena_request_slot(JSDualArena *da);

/* Per-reset request-allocator selection. Two regimes over the same
 * buffer (past the fixed state slot):
 *
 *   JS_ARENA_REQ_MODE_GC   — dlmalloc mspace: js_free reclaims,
 *     refcount + cycle GC live, ceiling = peak live set. Default.
 *   JS_ARENA_REQ_MODE_BUMP — bump cursor: ~3-instruction allocs,
 *     js_free is a no-op, GC off, ceiling = CUMULATIVE allocation
 *     (master semantics; OOM is the capacity signal — see oom_hit).
 *
 * js_dual_arena_set_request_mode() records the choice; it takes effect
 * at the NEXT reset (a request always runs entirely under one regime —
 * mid-request switching would orphan live allocations' provenance).
 * The intended production pattern: run handlers on BUMP for speed; on
 * oom_hit, retry the request under GC and tag the handler churny.
 * Switching costs nothing beyond the reset itself and touches no base
 * memory (mode state lives in the heap-allocated JSDualArena). */
typedef enum {
    JS_ARENA_REQ_MODE_GC = 0,
    JS_ARENA_REQ_MODE_BUMP = 1,
} JSArenaReqMode;
JS_EXTERN void          js_dual_arena_set_request_mode(JSDualArena *da,
                                                       JSArenaReqMode mode);
JS_EXTERN JSArenaReqMode js_dual_arena_request_mode(const JSDualArena *da);

/* JSMallocFunctions table; pass &js_dual_arena_malloc_funcs to JS_NewRuntime2
   together with a JSDualArena* as the opaque parameter. */
extern const JSMallocFunctions js_dual_arena_malloc_funcs;

/* Convenience wrappers around the above + JS_NewRuntime2 / JS_GetMallocOpaque. */
JS_EXTERN JSRuntime *JS_NewRuntimeArena(size_t base_size,
                                        size_t request_size);
JS_EXTERN void JS_FreezeRuntime(JSRuntime *rt);

/* Reports snapshot state that cannot be isolated per request: pending
 * promises, generators, async generators, and FinalizationRegistry.
 * Writes a human-readable report to `out_FILE` (a FILE*, or NULL for
 * stderr) naming each offender AND where it is reachable, e.g.
 *
 *   arenajs: the snapshot contains state that cannot be isolated per request:
 *     pending Promise        at globalThis.platform.warmup
 *                            resolve it before freezing, or create it per
 *                            request (a settled Promise is fine)
 *
 * Returns the number of offenders (0 = clean), or -1 if the scan itself
 * could not run. Cheap when clean: a class-id check per object, and the
 * graph walk only happens once something has been found.
 *
 * JS_FreezeRuntime calls this and aborts if it is non-zero. Call it
 * yourself BEFORE freezing to fail your own way — useful if you build
 * more than one snapshot (worker, offline sim, browser replay), since
 * otherwise the same mistake has to be diagnosed once per context.
 *
 * A SETTLED promise with no pending reactions is deliberately allowed:
 * Promise.resolve(x) as a memoised value is reasonable in a snapshot,
 * and its only base write is a debug-only flag. */
JS_EXTERN int JS_ScanSnapshotHazards(JSRuntime *rt, void *out_FILE);

/* Marks every base-resident ArrayBuffer immutable. Called by
   JS_FreezeRuntime: snapshot buffer bytes are base memory, so a write
   through any view would mutate the snapshot and be visible to every
   later request. Returns the number of buffers marked. Writes then
   throw TypeError ("ArrayBuffer is immutable"); copy explicitly
   (new Uint8Array(BASE_TA)) to get a mutable per-request one. */
JS_EXTERN int JS_MarkAllBaseArrayBuffersImmutable(JSRuntime *rt);
JS_EXTERN void JS_ResetRequestArena(JSRuntime *rt);
JS_EXTERN JSDualArena *JS_GetDualArena(JSRuntime *rt);

/* Internal: marks a runtime as arena-backed. Set by JS_NewRuntimeArena;
   defined in quickjs.c so it can reach the JSRuntime struct. */
JS_EXTERN void js_runtime_mark_arena(JSRuntime *rt);

/* Internal coordination — relocates the per-request mutable runtime state
   (current_exception, current_stack_frame, in_*, parent_promise) from its
   embedded backing on JSRuntime to a freshly allocated JSRequestState in
   the request arena. Called by JS_FreezeRuntime after the dual arena
   has flipped to request mode. Returns 0 on success, -1 on OOM. */
JS_EXTERN int JS_RelocateReqState(JSRuntime *rt);

/* arena: pre-force every JS_PROP_AUTOINIT property on every base JSObject
   so no lazy initialization remains by freeze time. Called by
   JS_FreezeRuntime BEFORE js_dual_arena_freeze (writes still land in
   base normally). Returns the number of autoinit properties forced.
   Eliminates the "first read of a base prototype method writes to base"
   class of post-freeze base mutations. */
JS_EXTERN int JS_ForceAllAutoinit(JSRuntime *rt);

/* arena: pre-mark every base prototype object's is_prototype flag so the
   first user code to use it as `__proto__` doesn't trigger a same-value
   write into base. Called by JS_FreezeRuntime; returns count marked. */
JS_EXTERN int JS_MarkAllPrototypes(JSRuntime *rt);

/* Hard enforcement of the inviolate-base invariant ("hard mprotect").
 * After freeze, js_dual_arena_harden() maps the base buffer PROT_READ
 * and installs a SIGSEGV handler that, for any write into a hardened
 * base range, prints the base offset (plus a backtrace on glibc) to
 * stderr and re-raises the default action (core dump). Where the
 * thermometer is a measuring instrument that FORGIVES base writes in
 * order to count them, this is a production tripwire: the MMU enforces
 * what the thermometer only measures.
 *
 * Mutually exclusive with the thermometer on the same arena — both own
 * the page protections. harden returns -1 if the thermometer is
 * enabled for this base range, and thermometer enable refuses a
 * hardened range.
 *
 * Teardown: js_dual_arena_free works while hardened (munmap ignores
 * page protections; it also deregisters the range). A refcount-walking
 * teardown (JS_FreeContext / JS_FreeRuntime) writes base and will trip
 * the handler — call js_dual_arena_unharden() first, or skip refcount
 * teardown and free the arena wholesale (the arena-smoke pattern).
 *
 * Host discipline under harden: configuration APIs that write base
 * (JS_SetInterruptHandler, JS_SetMaxStackSize, JS_SetGCThreshold,
 * JS_SetRuntimeOpaque, module-loader setters, ...) are pre-freeze
 * only. Per-request state (JS_SetDateNow / JS_SetTimeOrigin /
 * JS_SetRandomSeed) lands in JSRequestState and stays legal.
 *
 * Not available on WASM or ARENA_NO_THERM builds (no mprotect):
 * returns -1. */
JS_EXTERN int  js_dual_arena_harden(JSDualArena *da);
JS_EXTERN int  js_dual_arena_unharden(JSDualArena *da);
JS_EXTERN bool js_dual_arena_is_hardened(const JSDualArena *da);

/* Page-fault-based base-arena write detector — the "CoW thermometer" in
 * ARENA_PLAN.md. After enabling, the base arena buffer is mprotect'd
 * read-only and a SIGSEGV handler counts every write that touches it.
 * The handler marks the offending page in a bitmap, makes the page
 * writable, and returns so the faulting instruction succeeds.
 *
 * Workflow:
 *   js_dual_arena_freeze(da);
 *   js_arena_thermometer_enable();
 *   for each request:
 *     js_arena_thermometer_reset();
 *     ... run request ...
 *     printf("%zu pages, %zu writes\n",
 *            js_arena_thermometer_pages(),
 *            js_arena_thermometer_writes());
 *   js_arena_thermometer_disable();
 *
 * Returns 0 on success, -1 if base hasn't been frozen yet or if installing
 * the SIGSEGV handler / mprotect failed.
 *
 * Multiple ranges supported (cap: 8) — the handler dispatches by si_addr
 * to the entry that contains the faulting address. Each range gets its
 * own bitmap / baseline / counters. Two threads frobbing the same
 * arena's counters race; arena ownership is per-thread so this is an
 * embedder error.
 *
 * The handler uses mprotect, which is not strictly async-signal-safe per
 * POSIX but works on Linux/glibc in practice — same pattern as incremental
 * GCs. The previous SIGSEGV handler is chained for faults outside any
 * registered base range.
 */
JS_EXTERN int    js_arena_thermometer_enable(void);
JS_EXTERN void   js_arena_thermometer_disable(void);
JS_EXTERN void   js_arena_thermometer_reset(void);

/* Multi-runtime variants. The thermometer can track up to 8 ranges
 * concurrently; the SIGSEGV handler dispatches to the entry whose
 * (lo, hi) contains the faulting address. The no-arg API above
 * operates on the most recently enabled range. */
JS_EXTERN int    js_arena_thermometer_enable_range(const uint8_t *lo,
                                                   const uint8_t *hi);
JS_EXTERN void   js_arena_thermometer_disable_range(const uint8_t *lo,
                                                    const uint8_t *hi);
JS_EXTERN size_t js_arena_thermometer_pages(void);
JS_EXTERN size_t js_arena_thermometer_writes(void);
/* Fills `out` (capacity `cap`) with the byte offsets within the base
 * arena buffer of each dirtied page (multiples of the system page size).
 * Returns the total number of dirty pages, which may exceed cap (in
 * which case only the first cap entries were written). */
JS_EXTERN size_t js_arena_thermometer_dirty_offsets(size_t *out, size_t cap);
/* System page size at the time enable was called, or 0 if not enabled. */
JS_EXTERN size_t js_arena_thermometer_page_size(void);

/* Byte-level signal: enable mallocs a baseline copy of the entire base
 * buffer; these helpers memcmp the live base against the baseline to
 * count distinct mutated bytes. Useful to attribute residual writes
 * inside a single dirty page to specific structures.
 *   _changed_bytes()       — total across all dirty pages
 *   _changed_in_page(off)  — within the page starting at byte `off`
 */
JS_EXTERN size_t js_arena_thermometer_changed_bytes(void);
JS_EXTERN size_t js_arena_thermometer_changed_in_page(size_t page_offset);
/* Fills `out` with the byte offsets within the base buffer (NOT within the
 * page) of distinct changed bytes inside a single dirty page. Up to `cap`.
 * Returns the total number of changed bytes (which may exceed cap). */
JS_EXTERN size_t js_arena_thermometer_changed_byte_offsets(
    size_t page_offset, size_t *out, size_t cap);

/* Reads `len` bytes starting at `offset` in the baseline snapshot
 * (state at the most recent reset). Returns NULL if not enabled or
 * out of range. */
JS_EXTERN const void *js_arena_thermometer_baseline_at(size_t offset);

/* Diagnostic: when the SIGSEGV handler faults on an address inside
 * [base+lo_off, base+hi_off), prints a backtrace. Pass (0, 0) to disable.
 * Useful to identify which call paths still mutate base. */
JS_EXTERN void js_arena_thermometer_trace_range(size_t lo_off, size_t hi_off);

/* Diagnostic: prints JSRuntime field offsets to `out` so the thermometer's
 * dirty-page offsets can be cross-referenced with which mutable field is
 * being written. Layout is process-stable. */
JS_EXTERN void JS_DumpRuntimeOffsets(JSRuntime *rt, void *out_FILE);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_ARENA_H */
