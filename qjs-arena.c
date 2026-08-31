/*
 * QuickJS dual-region allocator implementation.
 *
 * Base region: bump arena, unchanged from the master branch. Buffer layout:
 *   bytes  0..8  : bump cursor (size_t)
 *   bytes  8..16 : padding, keeps payload 16-byte aligned
 *   bytes 16..   : allocations, each [ 8B size ][ 8B pad ][ payload ... ]
 * The cursor lives INSIDE the buffer (not in JSArena) so that a future
 * snapshot/restore step can memcpy the buffer and have the cursor relocate
 * automatically — same trick as ~/src/rove/src/qjs/snap.zig.
 *
 * Request region (hybrid-gc): a dlmalloc mspace created over the request
 * buffer at freeze time. HAVE_MMAP=0 + HAVE_MORECORE=0 confine it to the
 * buffer, so exhaustion returns NULL exactly like the bump ceiling did —
 * but js_free reclaims, so the ceiling is peak live set, not cumulative
 * allocation. Reset stays O(1): create_mspace_with_base over the same
 * dirty buffer stomps a fresh header and forgets every allocation; no
 * frees, no purge, pages stay resident. Determinism note: dlmalloc is
 * deterministic for a fixed call sequence over a fixed base, so the
 * "JSRequestState lands at the same address after every reset" invariant
 * (asserted in JS_RelocateReqState) holds just as it did for the bump
 * cursor.
 */
#include "qjs-arena.h"
#include "qjs-dlmalloc.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__wasm__) || defined(ARENA_NO_THERM)
/* No thermometer path: either WASM (no signals/page-protection/backtrace) or
   an explicit ARENA_NO_THERM opt-out (e.g. the rewind CLI / Windows, which
   never profile arena pages). The thermometer (mprotect+SIGSEGV based) is
   compiled out entirely; the arena buffer uses aligned_alloc instead of mmap
   since page alignment was only ever a thermometer requirement. */
#if defined(_WIN32)
#include <malloc.h>   /* _aligned_malloc / _aligned_free; mingw has no aligned_alloc */
#endif
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
/* backtrace()/backtrace_symbols_fd() are a glibc extension (execinfo.h); musl
   has no equivalent. The thermometer itself (mprotect+SIGSEGV) works on musl —
   only the optional, off-by-default symbolized backtrace is glibc-gated below. */
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

#define ARENA_ALIGN          16
#define ARENA_HEADER_SIZE    16   /* 8B size + 8B pad, keeps payload 16B-aligned */
#define ARENA_PREFIX_LEN     16   /* 8B cursor + 8B pad at the start of each buffer */
#define ARENA_DEFAULT_SIZE   (16u << 20)  /* 16 MiB */

typedef struct JSArena {
    uint8_t *buf;                /* owned, 16-byte aligned, capacity bytes */
    size_t capacity;
    size_t floor;                /* cursor start/reset point; > PREFIX_LEN
                                    when a head region is reserved (the
                                    request arena's fixed state slot) */
    void *last_alloc_ptr;        /* in-place realloc fast path */
    size_t last_alloc_aligned;
} JSArena;

typedef enum {
    JS_ARENA_MODE_BASE = 0,
    JS_ARENA_MODE_REQUEST = 1,
} JSArenaMode;

/* First request-mode allocation refused for lack of arena space.
   `hit` latches on the FIRST refusal (most informative — later ones
   are cascade). Cleared every js_dual_arena_reset_request. */
typedef struct {
    int    hit;
    size_t requested;
    size_t used;
    size_t limit;
} JSArenaOOM;

/* One provider extent of the request region. Nodes are libc-allocated
   (never inside the extent: dlmalloc owns every byte of a GC-mode
   extent, and the bump path wants the whole thing too). */
typedef struct JSReqChunk {
    struct JSReqChunk *next;
    uint8_t *buf;
    size_t size;
} JSReqChunk;

/* Built-in provider state: released extents are cached per arena and
   reused first-fit, so a request loop at steady state maps nothing. */
typedef struct JSReqCacheEntry {
    struct JSReqCacheEntry *next;
    uint8_t *buf;
    size_t size;
} JSReqCacheEntry;

typedef struct JSReqRegion {
    JSArenaChunkProvider prov;
    JSReqChunk *head;           /* prefix + state slot; never released */
    JSReqChunk *tail;           /* list runs head -> tail in acquisition order */
    size_t extents;             /* list length */
    size_t cap;                 /* budget for `held` */
    size_t held;                /* bytes across all extents, head included */
    size_t floor;               /* head offset past prefix + state slot */
    /* bump mode */
    JSReqChunk *cur;            /* extent being bumped */
    size_t cursor;              /* offset into cur */
    size_t bump_used;           /* cumulative aligned user bytes this request */
    void *last_alloc_ptr;       /* in-place realloc fast path */
    size_t last_alloc_aligned;
    /* MORECORE protocol: end of the extent last handed to dlmalloc */
    uint8_t *brk;
    /* built-in provider */
    JSReqCacheEntry *cache;
} JSReqRegion;

struct JSDualArena {
    JSArena base;
    JSReqRegion request;
    JSArenaMode mode;
    JSArenaOOM  oom;
    mspace msp;                 /* GC mode: mspace over buf+floor; NULL in bump mode */
    size_t request_used;        /* GC mode: live bytes (usable_size sums) */
    uint8_t req_mode;           /* JSArenaReqMode governing THIS request */
    uint8_t req_mode_next;      /* applied at the next reset (and at freeze) */
};

/* Per-thread list of registered arena base ranges; see qjs-arena.h. */
__thread struct js_arena_range js_arena_ranges[JS_ARENA_RANGES_MAX];
__thread int                   js_arena_range_count = 0;

/* Per-thread chunk set of registered request memory; see qjs-arena.h. */
__thread struct js_arena_chunk *js_arena_chunk_tab = NULL;
__thread uint32_t               js_arena_chunk_mask = 0;
__thread uint32_t               js_arena_chunk_count = 0;

int js_arena_register_base(const uint8_t *lo, const uint8_t *hi)
{
    if (js_arena_range_count >= JS_ARENA_RANGES_MAX)
        return -1;
    js_arena_ranges[js_arena_range_count].lo = lo;
    js_arena_ranges[js_arena_range_count].hi = hi;
    js_arena_range_count++;
    return 0;
}

void js_arena_unregister_base(const uint8_t *lo, const uint8_t *hi)
{
    for (int i = 0; i < js_arena_range_count; i++) {
        if (js_arena_ranges[i].lo == lo && js_arena_ranges[i].hi == hi) {
            /* Compact: move the last entry into this slot. */
            js_arena_range_count--;
            js_arena_ranges[i] = js_arena_ranges[js_arena_range_count];
            js_arena_ranges[js_arena_range_count].lo = NULL;
            js_arena_ranges[js_arena_range_count].hi = NULL;
            return;
        }
    }
}

#define JS_ARENA_CHUNK_TAB_INITIAL 64

static void chunk_tab_put(struct js_arena_chunk *tab, uint32_t mask,
                          uintptr_t key, JSDualArena *owner)
{
    uint32_t i = js_arena_chunk_hash(key, mask);
    while (tab[i].key && tab[i].key != key)
        i = (i + 1) & mask;
    tab[i].key = key;
    tab[i].owner = owner;
}

/* Rebuild the table at `new_size` slots (power of two), dropping every
   entry owned by `drop` (NULL drops nothing). */
static int chunk_tab_rebuild(uint32_t new_size, JSDualArena *drop)
{
    struct js_arena_chunk *nt = calloc(new_size, sizeof(*nt));
    if (!nt)
        return -1;
    uint32_t nmask = new_size - 1, kept = 0;
    if (js_arena_chunk_tab) {
        for (uint32_t i = 0; i <= js_arena_chunk_mask; i++) {
            struct js_arena_chunk *e = &js_arena_chunk_tab[i];
            if (e->key && e->owner && e->owner != drop) {
                chunk_tab_put(nt, nmask, e->key, e->owner);
                kept++;
            }
        }
        free(js_arena_chunk_tab);
    }
    js_arena_chunk_tab = nt;
    js_arena_chunk_mask = nmask;
    js_arena_chunk_count = kept;
    return 0;
}

int js_arena_register_request(const uint8_t *lo, const uint8_t *hi,
                              JSDualArena *owner)
{
    assert(((uintptr_t)lo & (JS_ARENA_CHUNK_SIZE - 1)) == 0);
    assert(((uintptr_t)hi & (JS_ARENA_CHUNK_SIZE - 1)) == 0);
    uintptr_t first = (uintptr_t)lo >> JS_ARENA_CHUNK_SHIFT;
    uintptr_t n = ((uintptr_t)hi >> JS_ARENA_CHUNK_SHIFT) - first;
    /* Grow ahead of insertion so load stays <= 1/2 for the whole batch. */
    size_t need = (size_t)js_arena_chunk_count + n;
    uint32_t size = js_arena_chunk_mask ? js_arena_chunk_mask + 1
                                        : JS_ARENA_CHUNK_TAB_INITIAL;
    while ((size_t)size < need * 2)
        size *= 2;
    if (size != js_arena_chunk_mask + 1) {
        if (chunk_tab_rebuild(size, NULL) < 0)
            return -1;
    }
    for (uintptr_t c = 0; c < n; c++)
        chunk_tab_put(js_arena_chunk_tab, js_arena_chunk_mask,
                      first + c + 1, owner);
    js_arena_chunk_count += (uint32_t)n;
    return 0;
}

/* Delete slot i from the linear-probing table by backward shift: walk
   the cluster after i, moving back any entry whose home slot is not
   cyclically inside (i, j], so every remaining entry stays reachable
   from its hash without tombstones. */
static void chunk_tab_del(uint32_t i)
{
    struct js_arena_chunk *tab = js_arena_chunk_tab;
    uint32_t mask = js_arena_chunk_mask;
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!tab[j].key)
            break;
        uint32_t h = js_arena_chunk_hash(tab[j].key, mask);
        bool stays = (i <= j) ? (h > i && h <= j) : (h > i || h <= j);
        if (!stays) {
            tab[i] = tab[j];
            i = j;
        }
    }
    tab[i].key = 0;
    tab[i].owner = NULL;
}

void js_arena_unregister_request_range(const uint8_t *lo, const uint8_t *hi)
{
    if (!js_arena_chunk_tab)
        return;
    uintptr_t first = (uintptr_t)lo >> JS_ARENA_CHUNK_SHIFT;
    uintptr_t n = ((uintptr_t)hi >> JS_ARENA_CHUNK_SHIFT) - first;
    uint32_t mask = js_arena_chunk_mask;
    for (uintptr_t c = 0; c < n; c++) {
        uintptr_t key = first + c + 1;
        uint32_t i = js_arena_chunk_hash(key, mask);
        while (js_arena_chunk_tab[i].key && js_arena_chunk_tab[i].key != key)
            i = (i + 1) & mask;
        if (!js_arena_chunk_tab[i].key)
            continue; /* not registered; tolerate */
        chunk_tab_del(i);
        js_arena_chunk_count--;
    }
    if (js_arena_chunk_count == 0) {
        free(js_arena_chunk_tab);
        js_arena_chunk_tab = NULL;
        js_arena_chunk_mask = 0;
    }
}

void js_arena_unregister_request(JSDualArena *owner)
{
    if (!js_arena_chunk_tab)
        return;
    uint32_t live = 0;
    for (uint32_t i = 0; i <= js_arena_chunk_mask; i++)
        if (js_arena_chunk_tab[i].key && js_arena_chunk_tab[i].owner &&
            js_arena_chunk_tab[i].owner != owner)
            live++;
    if (live == 0) {
        free(js_arena_chunk_tab);
        js_arena_chunk_tab = NULL;
        js_arena_chunk_mask = 0;
        js_arena_chunk_count = 0;
        return;
    }
    /* Rebuild without the owner's chunks (open addressing has no cheap
       delete). If the rebuild can't allocate, fall back to blanking the
       owner: stale keys stay in the table as never-matching entries
       (their owner is dead), which preserves probe chains at the cost
       of some load. Teardown is rare; correctness over thrift. */
    if (chunk_tab_rebuild(js_arena_chunk_mask + 1, owner) < 0) {
        for (uint32_t i = 0; i <= js_arena_chunk_mask; i++)
            if (js_arena_chunk_tab[i].owner == owner)
                js_arena_chunk_tab[i].owner = NULL;
    }
}

/* ----- low-level arena ops ----- */

static inline size_t arena_cursor(const JSArena *a)
{
    return *(const size_t *)a->buf;
}

static inline void arena_set_cursor(JSArena *a, size_t v)
{
    *(size_t *)a->buf = v;
}

/* Chunk-aligned block of `size` bytes (a chunk multiple), or NULL.
   Both regions and the built-in request provider come through here.
   The request chunk set (qjs-arena.h) keys on JS_ARENA_CHUNK_SIZE
   chunks and is only exact if blocks start and end on chunk
   boundaries; the thermometer additionally wants page-aligned base
   pages to mprotect, which the mmap path gives for free. */
static size_t pages_round(size_t size)
{
    size_t round = JS_ARENA_CHUNK_SIZE;
#if !defined(__wasm__) && !defined(ARENA_NO_THERM)
    /* mmap starts are page-aligned; pages are 4k/16k/64k — always a
       chunk multiple — so rounding to the larger keeps both exact. */
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz > 0 && (size_t)pagesz > round)
        round = (size_t)pagesz;
#endif
    return (size + round - 1) & ~(round - 1);
}

static void *pages_map(size_t size)
{
#if defined(__wasm__) || defined(ARENA_NO_THERM)
#if defined(_WIN32)
    void *buf = _aligned_malloc(size, JS_ARENA_CHUNK_SIZE); /* mingw: (size, align), no aligned_alloc */
#else
    void *buf = aligned_alloc(JS_ARENA_CHUNK_SIZE, size);
#endif
    if (buf)
        memset(buf, 0, size);
    return buf;
#else
    void *buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return buf == MAP_FAILED ? NULL : buf;
#endif
}

static void pages_unmap(void *buf, size_t size)
{
#if defined(__wasm__) || defined(ARENA_NO_THERM)
    (void)size;
#if defined(_WIN32)
    _aligned_free(buf);
#else
    free(buf);
#endif
#else
    munmap(buf, size);
#endif
}

static int arena_init(JSArena *a, size_t capacity)
{
    if (capacity == 0)
        capacity = ARENA_DEFAULT_SIZE;
    capacity = pages_round(capacity);
    if (capacity < ARENA_PREFIX_LEN + ARENA_HEADER_SIZE + ARENA_ALIGN)
        return -1;
    void *buf = pages_map(capacity);
    if (!buf)
        return -1;
    a->buf = buf;
    a->capacity = capacity;
    a->floor = ARENA_PREFIX_LEN;
    a->last_alloc_ptr = NULL;
    a->last_alloc_aligned = 0;
    arena_set_cursor(a, a->floor);
    return 0;
}

static void arena_destroy(JSArena *a)
{
    if (a->buf)
        pages_unmap(a->buf, a->capacity);
    memset(a, 0, sizeof(*a));
}

static void *arena_alloc(JSArena *a, size_t size)
{
    if (size == 0)
        return NULL;

    size_t aligned = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    size_t total = ARENA_HEADER_SIZE + aligned;
    size_t cursor = arena_cursor(a);
    if (cursor + total > a->capacity)
        return NULL;

    uint8_t *header = a->buf + cursor;
    *(uint64_t *)header = (uint64_t)size;
    /* second 8 bytes are padding, intentionally untouched */
    void *user_ptr = header + ARENA_HEADER_SIZE;
    arena_set_cursor(a, cursor + total);
    a->last_alloc_ptr = user_ptr;
    a->last_alloc_aligned = aligned;
    return user_ptr;
}

static inline uint64_t arena_user_size(const void *ptr)
{
    return *((const uint64_t *)ptr - 2);
}

static inline void arena_set_user_size(void *ptr, size_t size)
{
    *((uint64_t *)ptr - 2) = (uint64_t)size;
}

static void *arena_realloc(JSArena *a, void *ptr, size_t size)
{
    if (!ptr)
        return arena_alloc(a, size);
    if (size == 0)
        return NULL;

    size_t aligned = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    size_t old_size = (size_t)arena_user_size(ptr);

    if (ptr == a->last_alloc_ptr) {
        size_t old_aligned = a->last_alloc_aligned;
        size_t cursor = arena_cursor(a);
        if (aligned <= old_aligned) {
            /* shrink in place; return the freed tail to the bump region */
            arena_set_cursor(a, cursor - (old_aligned - aligned));
            a->last_alloc_aligned = aligned;
            arena_set_user_size(ptr, size);
            return ptr;
        }
        size_t delta = aligned - old_aligned;
        if (cursor + delta <= a->capacity) {
            arena_set_cursor(a, cursor + delta);
            a->last_alloc_aligned = aligned;
            arena_set_user_size(ptr, size);
            return ptr;
        }
        /* can't extend in place; fall through to copy */
    }

    void *np = arena_alloc(a, size);
    if (!np)
        return NULL;
    memcpy(np, ptr, size < old_size ? size : old_size);
    return np;
}

static bool arena_contains(const JSArena *a, const void *ptr)
{
    const uint8_t *p = ptr;
    return a->buf && p >= a->buf && p < a->buf + a->capacity;
}

/* ----- request region: provider extents ----- */

/* Built-in provider. Released policy-sized extents go to a per-arena
   cache and are handed out again before anything new is mapped; the
   match is exact-size because a block is charged to the budget at the
   size the arena asked for. Oversize extents (one allocation bigger
   than the policy size) are unmapped on release rather than cached —
   their sizes vary per request, so a cache of them would only grow. */
static void *default_acquire(void *opaque, size_t size)
{
    JSDualArena *da = opaque;
    JSReqCacheEntry **pp = &da->request.cache;
    for (; *pp; pp = &(*pp)->next) {
        JSReqCacheEntry *e = *pp;
        if (e->size == size) {
            *pp = e->next;
            void *buf = e->buf;
            free(e);
            return buf;
        }
    }
    return pages_map(size);
}

static void default_release(void *opaque, void *block, size_t size)
{
    JSDualArena *da = opaque;
    JSReqCacheEntry *e = size == JS_ARENA_REQ_EXTENT_DEFAULT ? malloc(sizeof(*e))
                                                            : NULL;
    if (!e) {
        pages_unmap(block, size);
        return;
    }
    e->buf = block;
    e->size = size;
    e->next = da->request.cache;
    da->request.cache = e;
}

static void default_cache_drain(JSDualArena *da)
{
    JSReqCacheEntry *e = da->request.cache;
    while (e) {
        JSReqCacheEntry *n = e->next;
        pages_unmap(e->buf, e->size);
        free(e);
        e = n;
    }
    da->request.cache = NULL;
}

/* Pull an extent of at least `need` bytes within the budget, register
   its chunks, append it to the list. NULL = refused (budget, provider
   or bookkeeping); the caller latches OOM. */
static JSReqChunk *req_acquire(JSDualArena *da, size_t need)
{
    JSReqRegion *r = &da->request;
    size_t size = pages_round(need);
    if (size < JS_ARENA_REQ_EXTENT_DEFAULT)
        size = JS_ARENA_REQ_EXTENT_DEFAULT;
    if (r->held + size > r->cap) {
        /* The policy extent doesn't fit; the exact need might. */
        size = pages_round(need);
        if (r->held + size > r->cap)
            return NULL;
    }
    JSReqChunk *c = malloc(sizeof(*c));
    if (!c)
        return NULL;
    uint8_t *buf = r->prov.acquire(r->prov.opaque, size);
    if (!buf) {
        free(c);
        return NULL;
    }
    assert(((uintptr_t)buf & (JS_ARENA_CHUNK_SIZE - 1)) == 0);
    if (js_arena_register_request(buf, buf + size, da) < 0) {
        r->prov.release(r->prov.opaque, buf, size);
        free(c);
        return NULL;
    }
    c->buf = buf;
    c->size = size;
    c->next = NULL;
    if (r->tail)
        r->tail->next = c;
    else
        r->head = c;
    r->tail = c;
    r->extents++;
    r->held += size;
    return c;
}

/* Hand every extent after the head back to the provider. */
static void req_release_extents(JSDualArena *da)
{
    JSReqRegion *r = &da->request;
    JSReqChunk *c = r->head ? r->head->next : NULL;
    while (c) {
        JSReqChunk *n = c->next;
        js_arena_unregister_request_range(c->buf, c->buf + c->size);
        r->prov.release(r->prov.opaque, c->buf, c->size);
        free(c);
        c = n;
    }
    if (r->head) {
        r->head->next = NULL;
        r->tail = r->head;
        r->extents = 1;
        r->held = r->head->size;
    } else {
        r->tail = NULL;
        r->extents = 0;
        r->held = 0;
    }
    r->brk = NULL;
}

/* Bump allocator over the extent list: bump `cur`; when it fills, take
   a fresh extent (the tail of the old one is simply abandoned until
   reset). Header format matches the base arena's so arena_user_size()
   reads either and cross-region realloc copies correctly. */
static void req_bump_reset(JSDualArena *da)
{
    JSReqRegion *r = &da->request;
    r->cur = r->head;
    r->cursor = r->floor;
    r->bump_used = 0;
    r->last_alloc_ptr = NULL;
    r->last_alloc_aligned = 0;
}

static void *req_bump_alloc(JSDualArena *da, size_t size)
{
    if (size == 0)
        return NULL;
    JSReqRegion *r = &da->request;
    size_t aligned = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    size_t total = ARENA_HEADER_SIZE + aligned;
    if (r->cursor + total > r->cur->size) {
        JSReqChunk *c = req_acquire(da, total);
        if (!c)
            return NULL;
        r->cur = c;
        r->cursor = 0;
    }
    uint8_t *header = r->cur->buf + r->cursor;
    *(uint64_t *)header = (uint64_t)size;
    /* second 8 bytes are padding, intentionally untouched */
    void *user_ptr = header + ARENA_HEADER_SIZE;
    r->cursor += total;
    r->bump_used += aligned;
    r->last_alloc_ptr = user_ptr;
    r->last_alloc_aligned = aligned;
    return user_ptr;
}

static void *req_bump_realloc(JSDualArena *da, void *ptr, size_t size)
{
    if (!ptr)
        return req_bump_alloc(da, size);
    if (size == 0)
        return NULL;
    JSReqRegion *r = &da->request;
    size_t aligned = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    size_t old_size = (size_t)arena_user_size(ptr);

    if (ptr == r->last_alloc_ptr) {
        size_t old_aligned = r->last_alloc_aligned;
        if (aligned <= old_aligned) {
            /* shrink in place; return the freed tail to the bump region */
            r->cursor -= old_aligned - aligned;
            r->bump_used -= old_aligned - aligned;
            r->last_alloc_aligned = aligned;
            arena_set_user_size(ptr, size);
            return ptr;
        }
        size_t delta = aligned - old_aligned;
        if (r->cursor + delta <= r->cur->size) {
            r->cursor += delta;
            r->bump_used += delta;
            r->last_alloc_aligned = aligned;
            arena_set_user_size(ptr, size);
            return ptr;
        }
        /* can't extend in place; fall through to copy */
    }

    void *np = req_bump_alloc(da, size);
    if (!np)
        return NULL;
    memcpy(np, ptr, size < old_size ? size : old_size);
    return np;
}

/* ----- dual arena ----- */

JSDualArena *js_dual_arena_new2(size_t base_size, size_t request_cap,
                                const JSArenaChunkProvider *prov)
{
    JSDualArena *da = calloc(1, sizeof(*da));
    if (!da)
        return NULL;
    if (arena_init(&da->base, base_size) < 0) {
        free(da);
        return NULL;
    }
    JSReqRegion *r = &da->request;
    if (prov) {
        r->prov = *prov;
    } else {
        r->prov.acquire = default_acquire;
        r->prov.release = default_release;
        r->prov.opaque = da;
    }
    if (request_cap == 0)
        request_cap = ARENA_DEFAULT_SIZE;
    r->cap = request_cap == SIZE_MAX ? SIZE_MAX : pages_round(request_cap);
    /* The fixed per-request state slot sits ahead of the allocator's
       territory in the head extent (see JS_ARENA_REQUEST_SLOT_SIZE). */
    r->floor = ARENA_PREFIX_LEN + JS_ARENA_REQUEST_SLOT_SIZE;
    size_t head = JS_ARENA_REQ_EXTENT_DEFAULT;
    if (head > r->cap)
        head = r->cap;
    if (head < r->floor + ARENA_HEADER_SIZE + ARENA_ALIGN
        || !req_acquire(da, head)) {
        arena_destroy(&da->base);
        free(da);
        return NULL;
    }
    /* Prefix mirrors the base layout so the head is a self-describing
       bump region pre-freeze too (nothing allocates there, but the
       request_used/oom accounting reads it uniformly). */
    memset(r->head->buf, 0, ARENA_PREFIX_LEN);
    req_bump_reset(da);
    da->mode = JS_ARENA_MODE_BASE;
    da->req_mode = JS_ARENA_REQ_MODE_GC;
    da->req_mode_next = JS_ARENA_REQ_MODE_GC;
    return da;
}

JSDualArena *js_dual_arena_new(size_t base_size, size_t request_cap)
{
    return js_dual_arena_new2(base_size, request_cap, NULL);
}

void js_dual_arena_free(JSDualArena *da)
{
    if (!da)
        return;
    const uint8_t *lo = da->base.buf;
    const uint8_t *hi = da->base.buf + da->base.capacity;
    /* If the thermometer is still active on this arena, disable it before
       tearing down — otherwise the SIGSEGV handler stays installed pointing
       into munmap'd memory. */
    js_arena_thermometer_disable_range(lo, hi);
    /* Hardened arenas can be freed directly (munmap ignores page
       protections) but the tripwire range must not outlive the
       mapping. */
    js_dual_arena_unharden(da);
    /* Drop this arena's ranges from the per-thread lists so a stale
       check after teardown doesn't read freed memory. No destroy_mspace:
       the mspace's only segment is our buffer (EXTERN_BIT), reclaimed by
       arena_destroy below. */
    js_arena_unregister_base(lo, hi);
    req_release_extents(da);
    if (da->request.head) {
        JSReqChunk *h = da->request.head;
        js_arena_unregister_request_range(h->buf, h->buf + h->size);
        da->request.prov.release(da->request.prov.opaque, h->buf, h->size);
        free(h);
        da->request.head = NULL;
    }
    js_arena_unregister_request(da); /* belt and braces: nothing left */
    default_cache_drain(da);
    arena_destroy(&da->base);
    free(da);
}

/* Apply the pending request mode: (re)establish the chosen allocator's
   discipline over the head extent past the fixed state slot. Only ever
   called with the head as the sole extent (creation, freeze, reset). */
static void request_mode_apply(JSDualArena *da)
{
    JSReqRegion *r = &da->request;
    assert(r->head && r->head->next == NULL);
    da->req_mode = da->req_mode_next;
    if (da->req_mode == JS_ARENA_REQ_MODE_GC) {
        da->msp = create_mspace_with_base(r->head->buf + r->floor,
                                          r->head->size - r->floor, 0);
        assert(da->msp); /* fails only if the head is absurdly small */
        da->request_used = 0;
    } else {
        da->msp = NULL;
        req_bump_reset(da);
    }
}

void js_dual_arena_freeze(JSDualArena *da)
{
    da->mode = JS_ARENA_MODE_REQUEST;
    request_mode_apply(da);
    js_arena_register_base(da->base.buf, da->base.buf + da->base.capacity);
    /* The head extent registered itself at creation; later extents
       register as they are acquired. */
}

void js_dual_arena_set_request_mode(JSDualArena *da, JSArenaReqMode mode)
{
    da->req_mode_next = (uint8_t)mode;
}

JSArenaReqMode js_dual_arena_request_mode(const JSDualArena *da)
{
    return (JSArenaReqMode)da->req_mode;
}

void js_dual_arena_reset_request(JSDualArena *da)
{
    /* Give back every extent but the head — O(extents), which for a
       typical request is one or a handful — then stomp a fresh mspace
       header over the head or rewind the bump cursor. This is also the
       only moment the request mode may change: every live request
       allocation dies here, so the next request's pointers all match
       the next request's mode. */
    req_release_extents(da);
    if (da->mode == JS_ARENA_MODE_REQUEST)
        request_mode_apply(da);
    else
        req_bump_reset(da); /* pre-freeze reset: still a bump region */
    da->oom.hit = 0;
    da->oom.requested = 0;
    da->oom.used = 0;
    da->oom.limit = 0;
}

/* Latch the first request-mode allocation refusal. Base-mode refusals
   are a build-the-snapshot problem, not a per-request capacity signal,
   so they're deliberately not recorded here. */
static void note_oom(JSDualArena *da, size_t requested)
{
    if (da->mode != JS_ARENA_MODE_REQUEST || da->oom.hit)
        return;
    da->oom.hit = 1;
    da->oom.requested = requested;
    /* GC mode: `used` is LIVE bytes — a refusal means the peak live set
       (plus fragmentation) hit capacity, a genuine sizing signal. Bump
       mode: `used` is cumulative — the classic churn ceiling, and the
       host's cue to retry the request under GC mode. */
    da->oom.used = (da->req_mode == JS_ARENA_REQ_MODE_GC)
        ? da->request_used
        : da->request.bump_used;
    da->oom.limit = da->request.cap == SIZE_MAX
        ? SIZE_MAX : da->request.cap - da->request.floor;
}

bool js_dual_arena_oom_hit(const JSDualArena *da)
{
    return da->oom.hit != 0;
}

size_t js_dual_arena_oom_requested(const JSDualArena *da)
{
    return da->oom.requested;
}

size_t js_dual_arena_oom_used(const JSDualArena *da)
{
    return da->oom.used;
}

size_t js_dual_arena_oom_limit(const JSDualArena *da)
{
    return da->oom.limit;
}

void *js_dual_arena_request_slot(JSDualArena *da)
{
    /* Fixed for the life of the arena; never handed to the allocator,
       never reclaimed by reset. */
    return da->request.head->buf + ARENA_PREFIX_LEN;
}

bool js_dual_arena_is_frozen(const JSDualArena *da)
{
    return da->mode == JS_ARENA_MODE_REQUEST;
}

bool js_dual_arena_in_base(const JSDualArena *da, const void *ptr)
{
    return arena_contains(&da->base, ptr);
}

bool js_dual_arena_in_request(const JSDualArena *da, const void *ptr)
{
    return js_arena_ptr_request_owner(ptr) == da;
}

size_t js_dual_arena_base_used(const JSDualArena *da)
{
    return arena_cursor(&da->base) - ARENA_PREFIX_LEN;
}

size_t js_dual_arena_request_used(const JSDualArena *da)
{
    /* GC mode: live bytes in the mspace. Bump mode (and pre-freeze):
       cursor offset past the floor — cumulative. */
    if (da->mode == JS_ARENA_MODE_REQUEST
        && da->req_mode == JS_ARENA_REQ_MODE_GC)
        return da->request_used;
    return da->request.bump_used;
}

size_t js_dual_arena_request_held(const JSDualArena *da)
{
    return da->request.held;
}

size_t js_dual_arena_request_extents(const JSDualArena *da)
{
    return da->request.extents;
}

/* ----- JSMallocFunctions glue -----
 *
 * Base mode (pre-freeze): bump into the base arena, free is a no-op —
 * unchanged from master; the snapshot build still wants append-only,
 * address-stable allocation.
 *
 * Request mode (post-freeze): dlmalloc mspace over the request region,
 * or the bump path. free/realloc must classify the pointer first:
 * base-arena pointers (immortal snapshot memory, bump headers) can still
 * reach these hooks — e.g. a realloc growing a structure allocated
 * pre-freeze — and must never be handed to the mspace.
 *
 * Growth: an mspace call that runs out of segments comes back to us
 * through MORECORE (js_arena_morecore). dlmalloc gives it no context,
 * so the arena that is allocating is parked in a thread-local for the
 * duration of the call — one TLS store per mspace call, and correct
 * with several arena runtimes interleaving on one thread. */

static __thread JSDualArena *morecore_da;

#define REQ_MFAIL ((void *)(uintptr_t)-1)   /* dlmalloc's MFAIL */

void *js_arena_morecore(ptrdiff_t n)
{
    JSDualArena *da = morecore_da;
    if (!da)
        return REQ_MFAIL;
    if (n == 0)
        return da->request.brk ? (void *)da->request.brk : REQ_MFAIL;
    if (n < 0)
        return REQ_MFAIL; /* MORECORE_CANNOT_TRIM: never asked */
    JSReqChunk *c = req_acquire(da, (size_t)n);
    if (!c)
        return REQ_MFAIL;
    da->request.brk = c->buf + c->size;
    return c->buf;
}

static void *jda_calloc(void *opaque, size_t count, size_t size)
{
    JSDualArena *da = opaque;
    if (count == 0 || size == 0)
        return NULL;
    if (count > (size_t)-1 / size)
        return NULL;
    size_t total = count * size;
    if (da->mode == JS_ARENA_MODE_BASE) {
        void *p = arena_alloc(&da->base, total);
        if (p)
            memset(p, 0, total);
        return p;
    }
    if (da->req_mode == JS_ARENA_REQ_MODE_BUMP) {
        /* bump memory is reused dirty across requests: always memset */
        void *p = req_bump_alloc(da, total);
        if (p)
            memset(p, 0, total);
        else
            note_oom(da, total);
        return p;
    }
    /* mspace_calloc memsets explicitly (its chunks are never fresh mmap
       pages here), so reused dirty buffer memory can't leak a previous
       request's bytes into zero-initialized allocations. */
    morecore_da = da;
    void *p = mspace_calloc(da->msp, count, size);
    if (p)
        da->request_used += mspace_usable_size(p);
    else
        note_oom(da, total);
    return p;
}

static void *jda_malloc(void *opaque, size_t size)
{
    JSDualArena *da = opaque;
    if (da->mode == JS_ARENA_MODE_BASE)
        return arena_alloc(&da->base, size);
    if (da->req_mode == JS_ARENA_REQ_MODE_BUMP) {
        void *p = req_bump_alloc(da, size);
        if (!p)
            note_oom(da, size);
        return p;
    }
    morecore_da = da;
    void *p = mspace_malloc(da->msp, size);
    if (p)
        da->request_used += mspace_usable_size(p);
    else
        note_oom(da, size);
    return p;
}

static void jda_free(void *opaque, void *ptr)
{
    JSDualArena *da = opaque;
    if (!ptr || da->mode == JS_ARENA_MODE_BASE)
        return;
    if (da->req_mode == JS_ARENA_REQ_MODE_BUMP)
        return; /* bump: free is a no-op; memory returns at reset */
    if (js_arena_ptr_request_owner(ptr) != da)
        return; /* base pointer: immortal, not an mspace chunk */
    da->request_used -= mspace_usable_size(ptr);
    mspace_free(da->msp, ptr);
}

static void *jda_realloc(void *opaque, void *ptr, size_t size)
{
    JSDualArena *da = opaque;
    if (da->mode == JS_ARENA_MODE_BASE) {
        return arena_realloc(&da->base, ptr, size);
    }
    if (da->req_mode == JS_ARENA_REQ_MODE_BUMP) {
        /* req_bump_realloc reads bump headers, which base pointers also
           carry, so cross-region growth copies correctly. */
        void *p = req_bump_realloc(da, ptr, size);
        if (!p && size != 0)
            note_oom(da, size);
        return p;
    }
    if (!ptr)
        return jda_malloc(opaque, size);
    if (size == 0) {
        /* Parity with the bump allocator: return NULL, leave ptr live.
           quickjs frees size==0 at its own layer, so this path is cold;
           not freeing here keeps any caller that treats NULL as failure
           from seeing its pointer die. */
        return NULL;
    }
    morecore_da = da;
    if (js_arena_ptr_request_owner(ptr) == da) {
        size_t old = mspace_usable_size(ptr);
        void *np = mspace_realloc(da->msp, ptr, size);
        if (!np) {
            note_oom(da, size);
            return NULL;
        }
        da->request_used += mspace_usable_size(np) - old;
        return np;
    }
    /* Growing a base (bump-header) allocation post-freeze: copy into the
       mspace, leave the immortal base bytes untouched. */
    size_t old_size = (size_t)arena_user_size(ptr);
    void *np = mspace_malloc(da->msp, size);
    if (!np) {
        note_oom(da, size);
        return NULL;
    }
    memcpy(np, ptr, size < old_size ? size : old_size);
    da->request_used += mspace_usable_size(np);
    return np;
}

static size_t jda_usable_size(const void *ptr)
{
    if (!ptr)
        return 0;
    /* No opaque on this hook — dispatch by pointer provenance AND the
       owning arena's current request mode: a request-range pointer is
       an mspace chunk in GC mode and a bump-header allocation in bump
       mode (all live request pointers match the current mode; the mode
       only changes at reset, which kills them all). Everything else
       (base arena, pre-freeze pointers) carries a bump header. */
    JSDualArena *owner = js_arena_ptr_request_owner(ptr);
    if (owner && owner->req_mode == JS_ARENA_REQ_MODE_GC)
        return mspace_usable_size(ptr);
    return (size_t)arena_user_size(ptr);
}

const JSMallocFunctions js_dual_arena_malloc_funcs = {
    jda_calloc,
    jda_malloc,
    jda_free,
    jda_realloc,
    jda_usable_size,
};

/* ----- convenience wrappers ----- */

JSRuntime *JS_NewRuntimeArena(size_t base_size, size_t request_size)
{
    JSDualArena *da = js_dual_arena_new(base_size, request_size);
    if (!da)
        return NULL;
    JSRuntime *rt = JS_NewRuntime2(&js_dual_arena_malloc_funcs, da);
    if (!rt) {
        js_dual_arena_free(da);
        return NULL;
    }
    /* Mark the runtime as arena-backed only at freeze time — pre-freeze
       it must behave as vanilla (allocate into base, use base shape_hash,
       no overlays), since request-arena machinery doesn't exist yet. */
    return rt;
}

JSDualArena *JS_GetDualArena(JSRuntime *rt)
{
    return (JSDualArena *)JS_GetMallocOpaque(rt);
}

void JS_FreezeRuntime(JSRuntime *rt)
{
    /* Order matters:
       1. Pre-force every autoinit property while we're still in BASE mode
          so the resulting writes land in base normally and no lazy init
          remains. Eliminates the prototype-method-after-reset hole.
       2. Pre-mark every base prototype's is_prototype flag so user-code
          uses of those objects as `__proto__` don't trigger a same-value
          write into snapshot memory.
       3. Flip the dual arena to request mode.
       4. Relocate JSRequestState into the request arena. */
    /* 0. Refuse a snapshot holding state that cannot be isolated per
       request. Done FIRST, and at freeze rather than at first use: a
       snapshot is built once at startup where whoever built it sees the
       error, whereas a first-use failure surfaces on a request and is
       attributed to a tenant who did nothing wrong. The scan reports
       where each offender is reachable — see JS_ScanSnapshotHazards. */
    if (JS_ScanSnapshotHazards(rt, NULL) > 0) {
        fprintf(stderr,
                "arenajs: refusing to freeze. Call JS_ScanSnapshotHazards() "
                "before JS_FreezeRuntime()\n         to detect this without "
                "aborting.\n");
        abort();
    }
    JS_ForceAllAutoinit(rt);
    JS_MarkAllPrototypes(rt);
    /* 2b. Snapshot ArrayBuffers become immutable: their bytes are base
       memory and cannot be shadowed cheaply (see the function's
       comment). Must happen while still in BASE mode. */
    JS_MarkAllBaseArrayBuffersImmutable(rt);
    js_dual_arena_freeze(JS_GetDualArena(rt));
    /* Flip rt->is_arena now: from this point on every chokepoint takes
       the arena code path. Done AFTER js_dual_arena_freeze because the
       dual arena registers its base range in the same step, so
       js_arena_ptr_is_base() works in concert with rt->is_arena. */
    js_runtime_mark_arena(rt);
    JS_RelocateReqState(rt);
}

void JS_ResetRequestArena(JSRuntime *rt)
{
    /* Rewind the cursor to the floor (past the fixed JSRequestState
       slot), then re-init the slot in place. rt->req (a one-time base
       write at freeze) stays valid with zero further base writes. */
    js_dual_arena_reset_request(JS_GetDualArena(rt));
    JS_RelocateReqState(rt);
}

/* ----- thermometer -----
 *
 * Counts writes to a base-arena range via mprotect+SIGSEGV. Supports
 * multiple ranges concurrently: each enabled arena gets its own
 * (bitmap, baseline, counters) entry; the SIGSEGV handler walks the
 * list to find which entry contains si_addr, and chains to the
 * previous handler if no entry matches.
 *
 * The signal handler is process-singleton (sigaction is process-wide).
 * Two threads frobbing the same arena's counters race; that's an
 * embedder error since arena ownership is per-thread.
 *
 * WASM: thermometer is compiled out (no mprotect, no signals). All
 * public symbols are stubbed at the bottom of the #else block so
 * embedders still link; thermometer_enable returns -1 on WASM so
 * callers see "not supported" cleanly.
 */

#if !defined(__wasm__) && !defined(ARENA_NO_THERM)

/* ----- hard mprotect (inviolate-base enforcement) ----- */

#define HARD_MAX 8
static struct { const uint8_t *lo, *hi; } hard_ranges[HARD_MAX];
static int hard_range_count = 0;
static volatile sig_atomic_t hard_handler_installed = 0;
static struct sigaction hard_prev_sa;

static int hard_find(const uint8_t *lo)
{
    for (int i = 0; i < hard_range_count; i++)
        if (hard_ranges[i].lo == lo)
            return i;
    return -1;
}

struct therm_state {
    const uint8_t *lo;
    const uint8_t *hi;
    size_t        base_pages;
    uint8_t      *dirty_bitmap;   /* one bit per page */
    uint8_t      *baseline;       /* copy of base buffer at enable time */
    size_t        writes;
    size_t        pages_dirty;
};

#define THERM_MAX 8
static struct therm_state therm_states[THERM_MAX];
static int therm_state_count = 0;
static volatile sig_atomic_t therm_handler_installed = 0;
static struct sigaction therm_prev_sa;
static long therm_page_size = 0;

/* Diagnostic: print a backtrace for faults whose address falls in
   [therm_trace_lo, therm_trace_hi). Off by default. */
static uintptr_t therm_trace_lo = 0;
static uintptr_t therm_trace_hi = 0;

static struct therm_state *therm_find_for(uintptr_t addr)
{
    for (int i = 0; i < therm_state_count; i++) {
        struct therm_state *s = &therm_states[i];
        if (addr >= (uintptr_t)s->lo && addr < (uintptr_t)s->hi)
            return s;
    }
    return NULL;
}

static struct therm_state *therm_find_by_lo(const uint8_t *lo)
{
    for (int i = 0; i < therm_state_count; i++) {
        if (therm_states[i].lo == lo)
            return &therm_states[i];
    }
    return NULL;
}

static void therm_chain(int sig, siginfo_t *info, void *ctx)
{
    if (therm_prev_sa.sa_flags & SA_SIGINFO) {
        if (therm_prev_sa.sa_sigaction)
            therm_prev_sa.sa_sigaction(sig, info, ctx);
    } else if (therm_prev_sa.sa_handler == SIG_DFL) {
        struct sigaction dfl = {0};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, NULL);
        raise(sig);
    } else if (therm_prev_sa.sa_handler != SIG_IGN
            && therm_prev_sa.sa_handler != NULL) {
        therm_prev_sa.sa_handler(sig);
    }
}

static void therm_sigsegv(int sig, siginfo_t *info, void *ctx)
{
    uintptr_t addr = (uintptr_t)info->si_addr;
    struct therm_state *s = therm_find_for(addr);
    if (!s) {
        therm_chain(sig, info, ctx);
        return;
    }

    s->writes++;
    size_t page_idx = (addr - (uintptr_t)s->lo) / (size_t)therm_page_size;
    size_t byte_idx = page_idx >> 3;
    uint8_t bit = (uint8_t)(1u << (page_idx & 7));
    if (!(s->dirty_bitmap[byte_idx] & bit)) {
        s->dirty_bitmap[byte_idx] |= bit;
        s->pages_dirty++;
    }

    if (therm_trace_lo && addr >= therm_trace_lo && addr < therm_trace_hi) {
        char header[128];
        int hlen = snprintf(header, sizeof(header),
                            "[therm] fault at base+%zu (addr=%p)\n",
                            addr - (uintptr_t)s->lo, (void *)addr);
        write(2, header, (size_t)hlen);
#if defined(__GLIBC__)
        void *frames[16];
        int nframes = backtrace(frames, 16);
        backtrace_symbols_fd(frames, nframes, 2);
#endif
    }

    void *page_addr = (void *)((uintptr_t)s->lo + page_idx * (size_t)therm_page_size);
    mprotect(page_addr, (size_t)therm_page_size, PROT_READ | PROT_WRITE);
}

static int therm_install_handler(void)
{
    if (therm_handler_installed)
        return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = therm_sigsegv;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &therm_prev_sa) < 0)
        return -1;
    therm_handler_installed = 1;
    return 0;
}

static void therm_uninstall_handler_if_idle(void)
{
    if (!therm_handler_installed || therm_state_count > 0)
        return;
    sigaction(SIGSEGV, &therm_prev_sa, NULL);
    therm_handler_installed = 0;
}

int js_arena_thermometer_enable_range(const uint8_t *lo, const uint8_t *hi)
{
    if (!lo || !hi || lo >= hi)
        return -1;
    if (therm_find_by_lo(lo))
        return 0; /* already enabled for this range */
    if (hard_find(lo) >= 0)
        return -1; /* hardened: the tripwire owns the protections */
    if (therm_state_count >= THERM_MAX)
        return -1;

    if (therm_page_size == 0) {
        therm_page_size = sysconf(_SC_PAGESIZE);
        if (therm_page_size <= 0)
            return -1;
    }
    if ((uintptr_t)lo & (uintptr_t)(therm_page_size - 1))
        return -1; /* mmap'd ranges are page-aligned */

    size_t base_size = (size_t)(hi - lo);
    size_t base_pages = base_size / (size_t)therm_page_size;
    size_t bitmap_bytes = (base_pages + 7) / 8;
    uint8_t *bitmap = calloc(1, bitmap_bytes ? bitmap_bytes : 1);
    if (!bitmap)
        return -1;
    uint8_t *baseline = malloc(base_size);
    if (!baseline) { free(bitmap); return -1; }
    memcpy(baseline, lo, base_size);

    if (therm_install_handler() < 0) {
        free(baseline); free(bitmap); return -1;
    }
    if (mprotect((void *)lo, base_size, PROT_READ) < 0) {
        free(baseline); free(bitmap);
        therm_uninstall_handler_if_idle();
        return -1;
    }

    struct therm_state *s = &therm_states[therm_state_count++];
    s->lo = lo; s->hi = hi;
    s->base_pages = base_pages;
    s->dirty_bitmap = bitmap;
    s->baseline = baseline;
    s->writes = 0;
    s->pages_dirty = 0;
    return 0;
}

void js_arena_thermometer_disable_range(const uint8_t *lo, const uint8_t *hi)
{
    (void)hi;
    struct therm_state *s = therm_find_by_lo(lo);
    if (!s)
        return;
    size_t base_size = (size_t)(s->hi - s->lo);
    mprotect((void *)s->lo, base_size, PROT_READ | PROT_WRITE);
    free(s->dirty_bitmap);
    free(s->baseline);
    /* compact: move the last entry into this slot */
    int idx = (int)(s - therm_states);
    therm_state_count--;
    if (idx != therm_state_count)
        therm_states[idx] = therm_states[therm_state_count];
    memset(&therm_states[therm_state_count], 0, sizeof(therm_states[0]));
    therm_uninstall_handler_if_idle();
}

/* No-arg API: operate on the most-recently-enabled state. Convenient
   for the typical single-runtime test harness; for multi-runtime
   debugging, call the _range variants explicitly. */
static struct therm_state *therm_current(void)
{
    return therm_state_count > 0 ? &therm_states[therm_state_count - 1] : NULL;
}

int js_arena_thermometer_enable(void)
{
    if (js_arena_range_count == 0)
        return -1; /* freeze hasn't published any range yet */
    /* Default to the most recently registered arena. */
    struct js_arena_range *r =
        &js_arena_ranges[js_arena_range_count - 1];
    return js_arena_thermometer_enable_range(r->lo, r->hi);
}

void js_arena_thermometer_disable(void)
{
    /* Disable everything the thermometer is tracking. Used by tests
       that toggle the thermometer once at end of run. */
    while (therm_state_count > 0) {
        struct therm_state *s = &therm_states[therm_state_count - 1];
        js_arena_thermometer_disable_range(s->lo, s->hi);
    }
}

void js_arena_thermometer_reset(void)
{
    struct therm_state *s = therm_current();
    if (!s)
        return;
    size_t base_size = (size_t)(s->hi - s->lo);
    mprotect((void *)s->lo, base_size, PROT_READ);
    if (s->baseline)
        memcpy(s->baseline, s->lo, base_size);
    size_t bitmap_bytes = (s->base_pages + 7) / 8;
    memset(s->dirty_bitmap, 0, bitmap_bytes);
    s->writes = 0;
    s->pages_dirty = 0;
}

size_t js_arena_thermometer_pages(void)
{
    struct therm_state *s = therm_current();
    return s ? s->pages_dirty : 0;
}

size_t js_arena_thermometer_writes(void)
{
    struct therm_state *s = therm_current();
    return s ? s->writes : 0;
}

void js_arena_thermometer_trace_range(size_t lo_off, size_t hi_off)
{
    if (lo_off == 0 && hi_off == 0) {
        therm_trace_lo = 0;
        therm_trace_hi = 0;
        return;
    }
    struct therm_state *s = therm_current();
    if (!s)
        return;
    therm_trace_lo = (uintptr_t)s->lo + lo_off;
    therm_trace_hi = (uintptr_t)s->lo + hi_off;
}

size_t js_arena_thermometer_page_size(void)
{
    return therm_state_count > 0 ? (size_t)therm_page_size : 0;
}

size_t js_arena_thermometer_dirty_offsets(size_t *out, size_t cap)
{
    struct therm_state *s = therm_current();
    if (!s)
        return 0;
    size_t found = 0;
    for (size_t i = 0; i < s->base_pages; i++) {
        if (s->dirty_bitmap[i >> 3] & (1u << (i & 7))) {
            if (found < cap)
                out[found] = i * (size_t)therm_page_size;
            found++;
        }
    }
    return found;
}

size_t js_arena_thermometer_changed_in_page(size_t page_offset)
{
    struct therm_state *s = therm_current();
    if (!s || !s->baseline)
        return 0;
    size_t base_size = (size_t)(s->hi - s->lo);
    if (page_offset >= base_size)
        return 0;
    size_t end = page_offset + (size_t)therm_page_size;
    if (end > base_size)
        end = base_size;
    const uint8_t *live = s->lo + page_offset;
    const uint8_t *base = s->baseline + page_offset;
    size_t n = 0;
    for (size_t i = 0, len = end - page_offset; i < len; i++)
        if (live[i] != base[i])
            n++;
    return n;
}

size_t js_arena_thermometer_changed_bytes(void)
{
    struct therm_state *s = therm_current();
    if (!s)
        return 0;
    size_t total = 0;
    for (size_t i = 0; i < s->base_pages; i++) {
        if (s->dirty_bitmap[i >> 3] & (1u << (i & 7)))
            total += js_arena_thermometer_changed_in_page(i * (size_t)therm_page_size);
    }
    return total;
}

const void *js_arena_thermometer_baseline_at(size_t offset)
{
    struct therm_state *s = therm_current();
    if (!s || !s->baseline)
        return NULL;
    size_t base_size = (size_t)(s->hi - s->lo);
    if (offset >= base_size)
        return NULL;
    return s->baseline + offset;
}

size_t js_arena_thermometer_changed_byte_offsets(
    size_t page_offset, size_t *out, size_t cap)
{
    struct therm_state *s = therm_current();
    if (!s || !s->baseline)
        return 0;
    size_t base_size = (size_t)(s->hi - s->lo);
    if (page_offset >= base_size)
        return 0;
    size_t end = page_offset + (size_t)therm_page_size;
    if (end > base_size)
        end = base_size;
    const uint8_t *live = s->lo;
    const uint8_t *base = s->baseline;
    size_t found = 0;
    for (size_t i = page_offset; i < end; i++) {
        if (live[i] != base[i]) {
            if (found < cap)
                out[found] = i;
            found++;
        }
    }
    return found;
}

/* ----- hard mprotect implementation -----
 *
 * SIGSEGV handler: faults inside a hardened base range get a
 * diagnostic (base offset + backtrace on glibc) and then the DEFAULT
 * action — we restore SIG_DFL and return, the faulting instruction
 * re-executes, and the process dies with an accurate core dump.
 * Foreign faults chain to whatever handler was installed before us
 * (ASan, the thermometer, the embedder's). */
static void hard_sigsegv(int sig, siginfo_t *info, void *ctx)
{
    uintptr_t addr = (uintptr_t)info->si_addr;
    for (int i = 0; i < hard_range_count; i++) {
        if (addr >= (uintptr_t)hard_ranges[i].lo
         && addr <  (uintptr_t)hard_ranges[i].hi) {
            char msg[128];
            int len = snprintf(msg, sizeof(msg),
                "[arena-harden] write to frozen base at base+%zu — aborting\n",
                addr - (uintptr_t)hard_ranges[i].lo);
            if (len > 0)
                write(2, msg, (size_t)len);
#if defined(__GLIBC__)
            void *frames[32];
            int nframes = backtrace(frames, 32);
            backtrace_symbols_fd(frames, nframes, 2);
#endif
            struct sigaction dfl;
            memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            sigaction(SIGSEGV, &dfl, NULL);
            return; /* re-executes the faulting store under SIG_DFL */
        }
    }
    /* not ours: previous handler */
    if (hard_prev_sa.sa_flags & SA_SIGINFO) {
        if (hard_prev_sa.sa_sigaction)
            hard_prev_sa.sa_sigaction(sig, info, ctx);
    } else if (hard_prev_sa.sa_handler == SIG_DFL) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, NULL);
        raise(sig);
    } else if (hard_prev_sa.sa_handler != SIG_IGN
            && hard_prev_sa.sa_handler != NULL) {
        hard_prev_sa.sa_handler(sig);
    }
}

int js_dual_arena_harden(JSDualArena *da)
{
    if (!da || da->mode != JS_ARENA_MODE_REQUEST)
        return -1; /* freeze first: the base range must be final */
    if (therm_find_by_lo(da->base.buf))
        return -1; /* thermometer owns the protections for this range */
    if (hard_find(da->base.buf) >= 0)
        return 0;  /* already hardened */
    if (hard_range_count >= HARD_MAX)
        return -1;
    if (!hard_handler_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_flags = SA_SIGINFO;
        sa.sa_sigaction = hard_sigsegv;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGSEGV, &sa, &hard_prev_sa) < 0)
            return -1;
        hard_handler_installed = 1;
    }
    if (mprotect(da->base.buf, da->base.capacity, PROT_READ) < 0) {
        if (hard_range_count == 0) {
            sigaction(SIGSEGV, &hard_prev_sa, NULL);
            hard_handler_installed = 0;
        }
        return -1;
    }
    hard_ranges[hard_range_count].lo = da->base.buf;
    hard_ranges[hard_range_count].hi = da->base.buf + da->base.capacity;
    hard_range_count++;
    return 0;
}

int js_dual_arena_unharden(JSDualArena *da)
{
    if (!da)
        return -1;
    int idx = hard_find(da->base.buf);
    if (idx < 0)
        return -1;
    mprotect(da->base.buf, da->base.capacity, PROT_READ | PROT_WRITE);
    hard_range_count--;
    if (idx != hard_range_count)
        hard_ranges[idx] = hard_ranges[hard_range_count];
    hard_ranges[hard_range_count].lo = NULL;
    hard_ranges[hard_range_count].hi = NULL;
    if (hard_range_count == 0 && hard_handler_installed) {
        sigaction(SIGSEGV, &hard_prev_sa, NULL);
        hard_handler_installed = 0;
    }
    return 0;
}

bool js_dual_arena_is_hardened(const JSDualArena *da)
{
    return da && hard_find(da->base.buf) >= 0;
}

#else /* defined(__wasm__) — thermometer stubs */

int  js_arena_thermometer_enable(void)                    { return -1; }
void js_arena_thermometer_disable(void)                   { }
void js_arena_thermometer_reset(void)                     { }
int  js_arena_thermometer_enable_range(const uint8_t *lo, const uint8_t *hi)
                                                          { (void)lo; (void)hi; return -1; }
void js_arena_thermometer_disable_range(const uint8_t *lo, const uint8_t *hi)
                                                          { (void)lo; (void)hi; }
size_t js_arena_thermometer_pages(void)                   { return 0; }
size_t js_arena_thermometer_writes(void)                  { return 0; }
size_t js_arena_thermometer_dirty_offsets(size_t *out, size_t cap)
                                                          { (void)out; (void)cap; return 0; }
size_t js_arena_thermometer_page_size(void)               { return 0; }
size_t js_arena_thermometer_changed_bytes(void)           { return 0; }
size_t js_arena_thermometer_changed_in_page(size_t off)   { (void)off; return 0; }
size_t js_arena_thermometer_changed_byte_offsets(size_t off, size_t *out, size_t cap)
                                                          { (void)off; (void)out; (void)cap; return 0; }
const void *js_arena_thermometer_baseline_at(size_t off)  { (void)off; return NULL; }
void js_arena_thermometer_trace_range(size_t lo, size_t hi)
                                                          { (void)lo; (void)hi; }
int  js_dual_arena_harden(JSDualArena *da)                { (void)da; return -1; }
int  js_dual_arena_unharden(JSDualArena *da)              { (void)da; return -1; }
bool js_dual_arena_is_hardened(const JSDualArena *da)     { (void)da; return false; }

#endif /* !defined(__wasm__) */
