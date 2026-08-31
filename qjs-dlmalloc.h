/*
 * Prototypes for the mspace subset of dlmalloc (dlmalloc.c, compiled via
 * qjs-dlmalloc.c with ONLY_MSPACES=1). An mspace is created over the
 * request region's head extent with create_mspace_with_base; HAVE_MMAP=0
 * and MORECORE = js_arena_morecore mean it grows only by asking the arena
 * for further extents, which the arena serves from its provider within
 * the request budget. A refused extent makes the allocation return NULL,
 * which propagates as JS OOM exactly like the bump arena's capacity
 * refusals did.
 *
 * Reset: the arena hands the non-head extents back to the provider, then
 * calls create_mspace_with_base over the (dirty) head again. All
 * allocator state lives inside the extents, so stomping a fresh header
 * forgets every prior allocation. The built-in provider caches released
 * extents, so a steady-state request loop maps nothing.
 */
#ifndef QJS_DLMALLOC_H
#define QJS_DLMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *mspace;

mspace create_mspace_with_base(void *base, size_t capacity, int locked);
size_t destroy_mspace(mspace msp);
void  *mspace_malloc(mspace msp, size_t bytes);
void   mspace_free(mspace msp, void *mem);
void  *mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void  *mspace_realloc(mspace msp, void *mem, size_t newsize);
size_t mspace_usable_size(const void *mem);

#ifdef __cplusplus
}
#endif

#endif /* QJS_DLMALLOC_H */
