/*
 * QuickJS dual bump-arena allocator implementation.
 *
 * Each arena owns a single contiguous buffer of fixed capacity. Allocations
 * never spill across arenas and never grow the buffer; an alloc beyond
 * capacity returns NULL (which propagates as JS OOM).
 *
 * Buffer layout:
 *   bytes  0..8  : bump cursor (size_t)
 *   bytes  8..16 : padding, keeps payload 16-byte aligned
 *   bytes 16..   : allocations, each [ 8B size ][ 8B pad ][ payload ... ]
 *
 * The cursor lives INSIDE the buffer (not in JSArena) so that a future
 * snapshot/restore step can memcpy the buffer and have the cursor relocate
 * automatically — same trick as ~/src/rove/src/qjs/snap.zig. Reset is one
 * store: write ARENA_PREFIX_LEN to the first 8 bytes.
 */
#include "qjs-arena.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN          16
#define ARENA_HEADER_SIZE    16   /* 8B size + 8B pad, keeps payload 16B-aligned */
#define ARENA_PREFIX_LEN     16   /* 8B cursor + 8B pad at the start of each buffer */
#define ARENA_DEFAULT_SIZE   (16u << 20)  /* 16 MiB */

typedef struct JSArena {
    uint8_t *buf;                /* owned, 16-byte aligned, capacity bytes */
    size_t capacity;
    void *last_alloc_ptr;        /* in-place realloc fast path */
    size_t last_alloc_aligned;
} JSArena;

typedef enum {
    JS_ARENA_MODE_BASE = 0,
    JS_ARENA_MODE_REQUEST = 1,
} JSArenaMode;

struct JSDualArena {
    JSArena base;
    JSArena request;
    JSArenaMode mode;
};

/* Process-global base-arena range; see qjs-arena.h. */
const uint8_t *js_arena_base_lo = NULL;
const uint8_t *js_arena_base_hi = NULL;

/* ----- low-level arena ops ----- */

static inline size_t arena_cursor(const JSArena *a)
{
    return *(const size_t *)a->buf;
}

static inline void arena_set_cursor(JSArena *a, size_t v)
{
    *(size_t *)a->buf = v;
}

static int arena_init(JSArena *a, size_t capacity)
{
    if (capacity == 0)
        capacity = ARENA_DEFAULT_SIZE;
    /* round capacity up to ARENA_ALIGN; aligned_alloc requires it */
    capacity = (capacity + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    if (capacity < ARENA_PREFIX_LEN + ARENA_HEADER_SIZE + ARENA_ALIGN)
        return -1;

    void *buf = aligned_alloc(ARENA_ALIGN, capacity);
    if (!buf)
        return -1;
    a->buf = buf;
    a->capacity = capacity;
    a->last_alloc_ptr = NULL;
    a->last_alloc_aligned = 0;
    arena_set_cursor(a, ARENA_PREFIX_LEN);
    return 0;
}

static void arena_destroy(JSArena *a)
{
    free(a->buf);
    memset(a, 0, sizeof(*a));
}

static void arena_reset(JSArena *a)
{
    arena_set_cursor(a, ARENA_PREFIX_LEN);
    a->last_alloc_ptr = NULL;
    a->last_alloc_aligned = 0;
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

/* ----- dual arena ----- */

JSDualArena *js_dual_arena_new(size_t base_size, size_t request_size)
{
    JSDualArena *da = calloc(1, sizeof(*da));
    if (!da)
        return NULL;
    if (arena_init(&da->base, base_size) < 0) {
        free(da);
        return NULL;
    }
    if (arena_init(&da->request, request_size) < 0) {
        arena_destroy(&da->base);
        free(da);
        return NULL;
    }
    da->mode = JS_ARENA_MODE_BASE;
    return da;
}

void js_dual_arena_free(JSDualArena *da)
{
    if (!da)
        return;
    /* Clear the global range if it pointed at this arena, so a stale
       check after teardown doesn't read freed memory. */
    if (js_arena_base_lo == da->base.buf) {
        js_arena_base_lo = NULL;
        js_arena_base_hi = NULL;
    }
    arena_destroy(&da->base);
    arena_destroy(&da->request);
    free(da);
}

void js_dual_arena_freeze(JSDualArena *da)
{
    da->mode = JS_ARENA_MODE_REQUEST;
    js_arena_base_lo = da->base.buf;
    js_arena_base_hi = da->base.buf + da->base.capacity;
}

void js_dual_arena_reset_request(JSDualArena *da)
{
    arena_reset(&da->request);
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
    return arena_contains(&da->request, ptr);
}

size_t js_dual_arena_base_used(const JSDualArena *da)
{
    return arena_cursor(&da->base) - ARENA_PREFIX_LEN;
}

size_t js_dual_arena_request_used(const JSDualArena *da)
{
    return arena_cursor(&da->request) - ARENA_PREFIX_LEN;
}

/* ----- JSMallocFunctions glue ----- */

static inline JSArena *active_arena(JSDualArena *da)
{
    return (da->mode == JS_ARENA_MODE_BASE) ? &da->base : &da->request;
}

static void *jda_calloc(void *opaque, size_t count, size_t size)
{
    if (count == 0 || size == 0)
        return NULL;
    if (count > (size_t)-1 / size)
        return NULL;
    size_t total = count * size;
    void *p = arena_alloc(active_arena(opaque), total);
    if (p)
        memset(p, 0, total);
    return p;
}

static void *jda_malloc(void *opaque, size_t size)
{
    return arena_alloc(active_arena(opaque), size);
}

static void jda_free(void *opaque, void *ptr)
{
    (void)opaque;
    (void)ptr;
}

static void *jda_realloc(void *opaque, void *ptr, size_t size)
{
    return arena_realloc(active_arena(opaque), ptr, size);
}

static size_t jda_usable_size(const void *ptr)
{
    if (!ptr)
        return 0;
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
    return rt;
}

JSDualArena *JS_GetDualArena(JSRuntime *rt)
{
    return (JSDualArena *)JS_GetMallocOpaque(rt);
}

void JS_FreezeRuntime(JSRuntime *rt)
{
    js_dual_arena_freeze(JS_GetDualArena(rt));
}

void JS_ResetRequestArena(JSRuntime *rt)
{
    js_dual_arena_reset_request(JS_GetDualArena(rt));
}
