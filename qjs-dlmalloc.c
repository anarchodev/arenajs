/*
 * dlmalloc configuration for the request-side allocator. dlmalloc.c is
 * vendored verbatim (Doug Lea, malloc-2.8.6, public domain); this TU sets
 * the configuration macros and includes it, so the upstream file carries
 * no local modifications.
 *
 *   ONLY_MSPACES  — export only the mspace_* API; no malloc/free
 *                   replacements, so the system allocator is untouched.
 *   HAVE_MMAP=0 + HAVE_MORECORE=1 (MORECORE = js_arena_morecore)
 *                 — the mspace starts over the request region's head
 *                   extent and grows only through the arena's provider;
 *                   a refused extent makes the allocation return NULL.
 *   USE_LOCKS=0   — arena runtimes are single-threaded by contract
 *                   (see the TLS range list in qjs-arena.h).
 */
#include "qjs-arena.h"   /* js_arena_morecore, JS_ARENA_CHUNK_SIZE */

#define ONLY_MSPACES 1
#define HAVE_MMAP 0
/* Growth goes through MORECORE, which the arena answers with provider
   extents (see js_arena_morecore in qjs-arena.h). The extents are not
   contiguous with each other, so dlmalloc takes its noncontiguous
   sys_alloc path and chains them as segments; MORECORE(0) reports the
   end of the extent just handed out, which is all that path needs.
   Trimming would hand memory back mid-request, which the arena does not
   do (everything returns at reset) — refuse it at compile time. */
#define HAVE_MORECORE 1
#define MORECORE js_arena_morecore
#define MORECORE_CONTIGUOUS 0
#define MORECORE_CANNOT_TRIM 1
/* Ask MORECORE in chunk multiples; the arena rounds up to its extent
   policy. dlmalloc's default here would be 64k, which merely over-asks. */
#define DEFAULT_GRANULARITY JS_ARENA_CHUNK_SIZE
/* HAVE_MREMAP 0 explicitly: dlmalloc otherwise takes its `#ifndef HAVE_MREMAP
   / #ifdef linux` branch, which sets HAVE_MREMAP 1 and #defines _GNU_SOURCE to
   expose mremap(). Every build here already passes -D_GNU_SOURCE, so that is a
   macro redefinition -- a warning normally, an error under QJS_BUILD_WERROR
   (the release job). mremap is unreachable with HAVE_MMAP 0 regardless. */
#define HAVE_MREMAP 0
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
