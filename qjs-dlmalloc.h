/*
 * Prototypes for the mspace subset of dlmalloc (dlmalloc.c, compiled via
 * qjs-dlmalloc.c with ONLY_MSPACES=1). An mspace created over the request
 * buffer with create_mspace_with_base — with HAVE_MMAP=0 and
 * HAVE_MORECORE=0 — can never allocate outside that buffer; exhaustion
 * returns NULL, which propagates as JS OOM exactly like the bump arena's
 * capacity refusals did.
 *
 * Reset is O(1): call create_mspace_with_base over the same (dirty)
 * buffer again. All allocator state lives inside the buffer, so stomping
 * a fresh header forgets every prior allocation in constant time. Nothing
 * is unmapped or purged; pages stay resident and warm across requests.
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
