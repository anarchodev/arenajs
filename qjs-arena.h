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

/* JSMallocFunctions table; pass &js_dual_arena_malloc_funcs to JS_NewRuntime2
   together with a JSDualArena* as the opaque parameter. */
extern const JSMallocFunctions js_dual_arena_malloc_funcs;

/* Convenience wrappers around the above + JS_NewRuntime2 / JS_GetMallocOpaque. */
JS_EXTERN JSRuntime *JS_NewRuntimeArena(size_t base_size,
                                        size_t request_size);
JS_EXTERN void JS_FreezeRuntime(JSRuntime *rt);
JS_EXTERN void JS_ResetRequestArena(JSRuntime *rt);
JS_EXTERN JSDualArena *JS_GetDualArena(JSRuntime *rt);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_ARENA_H */
