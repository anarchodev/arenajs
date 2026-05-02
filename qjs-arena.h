/*
 * QuickJS dual bump-arena allocator
 *
 * Two-arena model for request-scoped JS execution:
 *   - base arena: holds the snapshot (runtime, prelude, prototypes); never reset.
 *   - request arena: holds per-request allocations; reset between requests.
 *
 * Each arena owns a single contiguous buffer of fixed capacity, sized at
 * js_dual_arena_new() time. Allocations beyond capacity return NULL (which
 * propagates as JS OOM); the buffer never grows.
 *
 * The active arena is selected by a mode flag flipped via js_dual_arena_freeze().
 * Allocations are bump-pointer; js_free is a no-op; js_realloc extends in place
 * when the buffer is the most recent allocation, otherwise copies. Reset of
 * the request arena is one store: the bump cursor lives at offset 0 inside
 * the buffer.
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

/* Process-global base-arena address range, populated by js_dual_arena_freeze().
 * Used by the refcount inc/dec chokepoints to skip work for base-arena objects
 * (those allocations are immortal: they are alive for the runtime's lifetime,
 * inc/dec are no-ops, and the free path is never entered).
 *
 * Limitation: only one arena-backed runtime per process. Calling
 * js_dual_arena_freeze on a second dual arena overwrites the range and the
 * earlier runtime's chokepoint guards become wrong. Realistic deployments
 * for this allocator are single-runtime; revisit if that ever changes.
 */
extern const uint8_t *js_arena_base_lo;
extern const uint8_t *js_arena_base_hi;

static inline bool js_arena_ptr_is_base(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return b >= js_arena_base_lo && b < js_arena_base_hi;
}

/* JSMallocFunctions table; pass &js_dual_arena_malloc_funcs to JS_NewRuntime2
   together with a JSDualArena* as the opaque parameter. */
extern const JSMallocFunctions js_dual_arena_malloc_funcs;

/* Convenience wrappers around the above + JS_NewRuntime2 / JS_GetMallocOpaque. */
JS_EXTERN JSRuntime *JS_NewRuntimeArena(size_t base_size,
                                        size_t request_size);
JS_EXTERN void JS_FreezeRuntime(JSRuntime *rt);
JS_EXTERN void JS_ResetRequestArena(JSRuntime *rt);
JS_EXTERN JSDualArena *JS_GetDualArena(JSRuntime *rt);

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
 * Single-threaded only. The handler uses mprotect, which is not strictly
 * async-signal-safe per POSIX but works on Linux/glibc in practice — same
 * pattern as incremental GCs. The previous SIGSEGV handler is chained for
 * faults outside the base arena.
 */
JS_EXTERN int    js_arena_thermometer_enable(void);
JS_EXTERN void   js_arena_thermometer_disable(void);
JS_EXTERN void   js_arena_thermometer_reset(void);
JS_EXTERN size_t js_arena_thermometer_pages(void);
JS_EXTERN size_t js_arena_thermometer_writes(void);
/* Fills `out` (capacity `cap`) with the byte offsets within the base
 * arena buffer of each dirtied page (multiples of the system page size).
 * Returns the total number of dirty pages, which may exceed cap (in
 * which case only the first cap entries were written). */
JS_EXTERN size_t js_arena_thermometer_dirty_offsets(size_t *out, size_t cap);
/* System page size at the time enable was called, or 0 if not enabled. */
JS_EXTERN size_t js_arena_thermometer_page_size(void);

/* Diagnostic: prints JSRuntime field offsets to `out` so the thermometer's
 * dirty-page offsets can be cross-referenced with which mutable field is
 * being written. Layout is process-stable. */
JS_EXTERN void JS_DumpRuntimeOffsets(JSRuntime *rt, void *out_FILE);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_ARENA_H */
