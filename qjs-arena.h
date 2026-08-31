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
 * The base region is a single contiguous buffer of fixed capacity, sized
 * at js_dual_arena_new() time; it never grows.
 *
 * The request region is a list of chunk-aligned extents pulled from a
 * provider on demand (see JSArenaChunkProvider). A head extent, acquired
 * at creation, holds the fixed per-request state slot and is never
 * released; further extents are acquired as the request grows — by
 * dlmalloc through MORECORE in GC mode, by the bump path when its
 * current extent fills — and handed back at reset. A total byte budget
 * (`request_cap`) bounds what a request may hold; a refused acquisition
 * returns NULL (which propagates as JS OOM) and latches the OOM record.
 *
 * The active region is selected by a mode flag flipped via
 * js_dual_arena_freeze(), which also creates the mspace over the head
 * extent. Reset releases the non-head extents and stomps a fresh mspace
 * header over the head — every prior allocation is forgotten, and the
 * default provider caches released extents so a steady-state request
 * loop makes no system calls.
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
/* One request's memory: its extents, allocator state, OOM record and
 * mode. A JSDualArena owns one base region and any number of these;
 * exactly one (or none) is *selected* at a time and receives new
 * allocations. Frees and reallocs route to the arena that owns the
 * pointer regardless of selection. The fixed JSRequestState slot lives
 * in each request arena's head extent, so selecting a request arena is
 * also what puts its state in front of the runtime (see
 * js_dual_arena_state_cell). */
typedef struct JSRequestArena JSRequestArena;

/* Source of request-region extents. `acquire` returns a block of
 * exactly `size` bytes (a multiple of JS_ARENA_CHUNK_SIZE) aligned to
 * JS_ARENA_CHUNK_SIZE, or NULL to refuse — refusal is the host's
 * capacity policy and surfaces as a JS OOM plus the oom_* record.
 * `release` takes back a block previously returned by `acquire`, with
 * the same `size`. Both are called on the runtime's thread, synchronously
 * from inside an allocation (acquire) or from reset / teardown
 * (release). A provider that hands out cached blocks must not zero
 * them; the arena treats every byte past its own bookkeeping as dirty. */
typedef struct JSArenaChunkProvider {
    void *(*acquire)(void *opaque, size_t size);
    void  (*release)(void *opaque, void *block, size_t size);
    void  *opaque;
} JSArenaChunkProvider;

/* Extent size the arena asks the provider for when it needs more than
 * it holds, unless a single allocation needs more (then that, rounded
 * up to a chunk). The head extent is this size too, capped at
 * request_cap. */
#define JS_ARENA_REQ_EXTENT_DEFAULT ((size_t)256 << 10)

/* base_size: fixed base capacity. request_cap: the most request memory
 * a single request may hold across all its extents (the pre-chunking
 * `request_size`; oom_limit reports it). Pass 0 for a 16 MiB default in
 * either; SIZE_MAX for an unbounded request budget (the provider is
 * then the only limit). `prov` NULL selects the built-in provider:
 * page-mapped extents with a per-dual-arena cache of released ones.
 * Creates and selects one request arena with that budget/provider —
 * the single-request embedding needs nothing more. */
JS_EXTERN JSDualArena *js_dual_arena_new2(size_t base_size,
                                          size_t request_cap,
                                          const JSArenaChunkProvider *prov);
/* js_dual_arena_new2(base_size, request_cap, NULL). */
JS_EXTERN JSDualArena *js_dual_arena_new(size_t base_size,
                                         size_t request_cap);
/* Frees every request arena still owned, then the base. */
JS_EXTERN void js_dual_arena_free(JSDualArena *da);

/* ----- request arenas -----
 *
 * A request arena may be created before or after freeze; one created
 * after freeze is immediately usable once selected. Freeing the
 * selected arena deselects it (no allocation may happen until another
 * is selected). Values never cross request arenas: only base memory is
 * shared, so a pointer allocated under one request arena must not be
 * reachable from JS run under another — that is the host's contract,
 * exactly as it was between consecutive resets. A fresh arena's state
 * slot is uninitialised until JS_RelocateReqState runs on it. */
JS_EXTERN JSRequestArena *js_request_arena_new(JSDualArena *da,
                                               size_t request_cap,
                                               const JSArenaChunkProvider *prov);
JS_EXTERN void js_request_arena_free(JSRequestArena *ra);
JS_EXTERN JSDualArena *js_request_arena_dual(const JSRequestArena *ra);
/* Make `ra` (NULL = none) the arena that receives allocations and
 * whose state slot the runtime reads. Zero base writes. */
JS_EXTERN void js_dual_arena_select_request(JSDualArena *da,
                                            JSRequestArena *ra);
JS_EXTERN JSRequestArena *js_dual_arena_current_request(const JSDualArena *da);
/* Heap cell (never base, never request memory) holding the selected
 * request's JSRequestState*. The runtime points its req_cell here at
 * freeze — one base write, ever — and every later request switch
 * rewrites this cell instead of the hardened JSRuntime. */
JS_EXTERN void **js_dual_arena_state_cell(JSDualArena *da);
JS_EXTERN void *js_request_arena_slot(JSRequestArena *ra);
/* Per-request-arena forms of the js_dual_arena_* request calls below
 * (which act on the selected arena). */
JS_EXTERN void   js_request_arena_reset(JSRequestArena *ra);
JS_EXTERN bool   js_request_arena_contains(const JSRequestArena *ra,
                                           const void *ptr);
JS_EXTERN size_t js_request_arena_used(const JSRequestArena *ra);
JS_EXTERN size_t js_request_arena_held(const JSRequestArena *ra);
JS_EXTERN size_t js_request_arena_extents(const JSRequestArena *ra);
JS_EXTERN bool   js_request_arena_oom_hit(const JSRequestArena *ra);
JS_EXTERN size_t js_request_arena_oom_requested(const JSRequestArena *ra);
JS_EXTERN size_t js_request_arena_oom_used(const JSRequestArena *ra);
JS_EXTERN size_t js_request_arena_oom_limit(const JSRequestArena *ra);

JS_EXTERN void js_dual_arena_freeze(JSDualArena *da);
/* Reset the SELECTED request arena. */
JS_EXTERN void js_dual_arena_reset_request(JSDualArena *da);
JS_EXTERN bool js_dual_arena_is_frozen(const JSDualArena *da);

JS_EXTERN bool js_dual_arena_in_base(const JSDualArena *da, const void *ptr);
/* True if `ptr` belongs to ANY request arena of `da`. */
JS_EXTERN bool js_dual_arena_in_request(const JSDualArena *da, const void *ptr);

JS_EXTERN size_t js_dual_arena_base_used(const JSDualArena *da);
/* GC mode: live bytes. Bump mode: cumulative bytes allocated this request. */
JS_EXTERN size_t js_dual_arena_request_used(const JSDualArena *da);
/* Bytes of provider extents the request region currently holds (head
 * included); what a parked request costs. Counts against request_cap. */
JS_EXTERN size_t js_dual_arena_request_held(const JSDualArena *da);
/* Number of extents currently held (>= 1 once created). */
JS_EXTERN size_t js_dual_arena_request_extents(const JSDualArena *da);

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

/* Request-region membership: a per-thread hash set of chunk indices.
 *
 * Needed because js_malloc_usable_size receives only the pointer — no
 * opaque — and must dispatch between mspace chunks (mspace_usable_size)
 * and bump-header allocations; the owning arena's CURRENT request mode
 * decides, and every live request pointer matches it because a reset
 * (the only moment the mode can change) kills all request allocations
 * wholesale.
 *
 * Unlike the base list above this is NOT a bounded range table: request
 * memory is registered one JS_ARENA_CHUNK_SIZE chunk at a time, keyed on
 * (addr >> JS_ARENA_CHUNK_SHIFT), so a request region can be any number
 * of non-contiguous chunks and membership stays O(1) regardless of how
 * many are registered. Request buffers are chunk-aligned and chunk-sized
 * (arena_init rounds them) so the set is exact — no foreign heap pointer
 * shares a chunk with request memory. Storage is libc-malloc'd, sized to
 * keep the load factor <= 1/2; it lives outside base and outside any
 * request region. Registering is O(chunks) and happens at freeze;
 * lookups are a multiply, a mask and a probe. */
#define JS_ARENA_CHUNK_SHIFT 12
#define JS_ARENA_CHUNK_SIZE  ((size_t)1 << JS_ARENA_CHUNK_SHIFT)
struct js_arena_chunk {
    uintptr_t      key;    /* chunk index + 1; 0 = empty slot */
    JSRequestArena *owner;
};
extern JS_EXTERN __thread struct js_arena_chunk *js_arena_chunk_tab;
extern JS_EXTERN __thread uint32_t               js_arena_chunk_mask;  /* size - 1; 0 = no table */
extern JS_EXTERN __thread uint32_t               js_arena_chunk_count;

static inline uint32_t js_arena_chunk_hash(uintptr_t key, uint32_t mask)
{
    uint64_t v = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(v >> 32) & mask;
}

/* Owning request arena of a pointer, or NULL for anything else (base,
 * pre-freeze, vanilla heap). */
static inline JSRequestArena *js_arena_ptr_request_owner(const void *p)
{
    struct js_arena_chunk *tab = js_arena_chunk_tab;
    if (!tab)
        return NULL;
    uintptr_t key = ((uintptr_t)p >> JS_ARENA_CHUNK_SHIFT) + 1;
    uint32_t mask = js_arena_chunk_mask;
    uint32_t i = js_arena_chunk_hash(key, mask);
    for (;;) {
        uintptr_t k = tab[i].key;
        if (k == 0)
            return NULL;
        if (k == key)
            return tab[i].owner;
        i = (i + 1) & mask;
    }
}

static inline bool js_arena_ptr_is_request(const void *p)
{
    return js_arena_ptr_request_owner(p) != NULL;
}

/* Register every chunk of [lo, hi) — both chunk-aligned — as owned by
 * `owner`. Returns 0, or -1 if the table could not grow (malloc failure);
 * on -1 nothing was registered. Unregister drops every chunk owned by
 * `owner` and frees the table when it empties. */
JS_EXTERN int  js_arena_register_request(const uint8_t *lo, const uint8_t *hi,
                                         JSRequestArena *owner);
/* Drop the chunks of [lo, hi) — an extent going back to the provider.
 * O(chunks in the range); the table is freed when it empties. */
JS_EXTERN void js_arena_unregister_request_range(const uint8_t *lo,
                                                 const uint8_t *hi);
JS_EXTERN void js_arena_unregister_request(JSRequestArena *owner);

/* dlmalloc's MORECORE for the request mspace (qjs-dlmalloc.c wires it
 * in). sbrk protocol: n > 0 acquires an extent of at least n bytes from
 * the request arena currently allocating and returns its start; n == 0 returns
 * the end of the extent most recently handed out; n < 0 (trim) is
 * refused. Returns (void *)-1 on refusal, as dlmalloc expects. */
JS_EXTERN void *js_arena_morecore(ptrdiff_t n);

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
/* Reset the CURRENT request (no-op if none is entered), then re-init its
 * state: the run-to-completion path. */
JS_EXTERN void JS_ResetRequestArena(JSRuntime *rt);
JS_EXTERN JSDualArena *JS_GetDualArena(JSRuntime *rt);

/* ----- requests as objects -----
 *
 * A frozen runtime runs one request at a time but may hold any number:
 * a request that returned to the host with work pending (a promise
 * awaiting a host operation, jobs not yet drained) keeps its memory
 * and its state until the host enters it again. Enter and leave only
 * ever happen at a job boundary — with no JS frame live — which is
 * where the C stack is empty; JS_EnterRequest / JS_LeaveRequest refuse
 * (-1) if called from inside JS. Values never cross requests: only
 * base is shared. With no request entered the runtime reads the
 * pristine template state and accepts no allocation.
 *
 * JS_NewRequest creates a request arena with the given budget /
 * provider (see js_dual_arena_new2) and allocator mode, and initialises
 * its state; the entered request, if any, is left entered. The request
 * created by JS_NewRuntimeArena is entered at freeze, so a
 * single-request embedder never sees this API. */
JS_EXTERN JSRequestArena *JS_NewRequest(JSRuntime *rt, size_t request_cap,
                                        const JSArenaChunkProvider *prov,
                                        JSArenaReqMode mode);
JS_EXTERN int  JS_EnterRequest(JSRuntime *rt, JSRequestArena *req);
JS_EXTERN int  JS_LeaveRequest(JSRuntime *rt);
JS_EXTERN JSRequestArena *JS_CurrentRequest(JSRuntime *rt);
/* Frees the request's memory. Leaves it first if it is entered; refuses
 * (-1) from inside JS. */
JS_EXTERN int  JS_FreeRequest(JSRuntime *rt, JSRequestArena *req);

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
