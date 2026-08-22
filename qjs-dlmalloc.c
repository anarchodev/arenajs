/*
 * dlmalloc configuration for the request-side allocator. dlmalloc.c is
 * vendored verbatim (Doug Lea, malloc-2.8.6, public domain); this TU sets
 * the configuration macros and includes it, so the upstream file carries
 * no local modifications.
 *
 *   ONLY_MSPACES  — export only the mspace_* API; no malloc/free
 *                   replacements, so the system allocator is untouched.
 *   HAVE_MMAP=0 + HAVE_MORECORE=0
 *                 — the mspace can never grow beyond the buffer handed to
 *                   create_mspace_with_base; exhaustion returns NULL.
 *   USE_LOCKS=0   — arena runtimes are single-threaded by contract
 *                   (see the TLS range list in qjs-arena.h).
 */
#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 0
#define NO_MALLOC_STATS 1

/* Sanitizer builds: ASan cannot see use-after-free INSIDE the mspace
   buffer (it doesn't track a custom allocator's chunks), so turn on
   FOOTERS there — a magic-tagged footer per chunk, verified on
   free/realloc in O(1). A UAF that stomps chunk metadata trips it
   instead of silently corrupting the heap.

   dlmalloc's DEBUG (full internal consistency walks per operation) is
   deliberately NOT tied to sanitizers: it made an ASan test262 soak
   ~20x slower (bin traversals × hundreds of thousands of ops per
   test). Opt in explicitly with -DQJS_DLMALLOC_DEEP_CHECK when
   bisecting a specific corruption. */
#if defined(__SANITIZE_ADDRESS__)
#define QJS_DLMALLOC_CHECKED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QJS_DLMALLOC_CHECKED 1
#endif
#endif
#ifdef QJS_DLMALLOC_CHECKED
#define FOOTERS 1
#endif
#ifdef QJS_DLMALLOC_DEEP_CHECK
#define DEBUG 1
#endif

#if defined(__wasm__)
/* WASI libc has these headers, but nothing mmap-shaped is reachable with
   HAVE_MMAP=0; keep the include surface minimal. */
#define LACKS_SYS_MMAN_H 1
#endif

/* dlmalloc computes TOP_FOOT_SIZE via align_offset(chunk2mem(0)) -- pointer
   arithmetic on NULL, which clang rejects under -Werror
   (-Wgnu-null-pointer-arithmetic; hit by the emscripten CI job). The idiom is
   benign and dlmalloc.c is vendored verbatim, so suppress it here in the
   wrapper rather than diverge from upstream. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-null-pointer-arithmetic"
#endif

#include "dlmalloc.c"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
