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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
    /* round capacity up to page size — we use mmap for page-aligned starts
       so the thermometer can mprotect this buffer without affecting any
       neighbouring allocation. */
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0)
        pagesz = 4096;
    capacity = (capacity + (size_t)pagesz - 1) & ~(size_t)(pagesz - 1);
    if (capacity < ARENA_PREFIX_LEN + ARENA_HEADER_SIZE + ARENA_ALIGN)
        return -1;

    void *buf = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED)
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
    if (a->buf)
        munmap(a->buf, a->capacity);
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
    /* If the thermometer is still active on this arena, disable it before
       tearing down — otherwise the SIGSEGV handler stays installed pointing
       into munmap'd memory. */
    if (js_arena_base_lo == da->base.buf)
        js_arena_thermometer_disable();
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
    /* Order matters: flip to request mode FIRST so the JSRequestState
       allocation lands in the request arena, then relocate. */
    js_dual_arena_freeze(JS_GetDualArena(rt));
    JS_RelocateReqState(rt);
}

void JS_ResetRequestArena(JSRuntime *rt)
{
    js_dual_arena_reset_request(JS_GetDualArena(rt));
}

/* ----- thermometer -----
 *
 * Counts writes to the base arena via mprotect+SIGSEGV. The handler chains
 * to the previous handler for faults outside base. Single-threaded.
 */

static volatile sig_atomic_t therm_enabled = 0;
static struct sigaction therm_prev_sa;
static long therm_page_size = 0;
static size_t therm_base_pages = 0;
static uint8_t *therm_dirty_bitmap = NULL;   /* one bit per page, 1 = dirtied since last reset */
static uint8_t *therm_baseline = NULL;       /* copy of base buffer at enable time */
static size_t therm_writes = 0;
static size_t therm_pages_dirty = 0;

static void therm_chain(int sig, siginfo_t *info, void *ctx)
{
    if (therm_prev_sa.sa_flags & SA_SIGINFO) {
        if (therm_prev_sa.sa_sigaction)
            therm_prev_sa.sa_sigaction(sig, info, ctx);
    } else if (therm_prev_sa.sa_handler == SIG_DFL) {
        /* restore default and re-raise so we get a proper crash */
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
    if (!therm_enabled) {
        therm_chain(sig, info, ctx);
        return;
    }
    uintptr_t addr = (uintptr_t)info->si_addr;
    uintptr_t lo = (uintptr_t)js_arena_base_lo;
    uintptr_t hi = (uintptr_t)js_arena_base_hi;
    if (addr < lo || addr >= hi) {
        therm_chain(sig, info, ctx);
        return;
    }

    therm_writes++;
    size_t page_idx = (addr - lo) / (size_t)therm_page_size;
    size_t byte_idx = page_idx >> 3;
    uint8_t bit = (uint8_t)(1u << (page_idx & 7));
    if (!(therm_dirty_bitmap[byte_idx] & bit)) {
        therm_dirty_bitmap[byte_idx] |= bit;
        therm_pages_dirty++;
    }

    void *page_addr = (void *)(lo + page_idx * (size_t)therm_page_size);
    /* mprotect is not on POSIX's async-signal-safe list, but is in practice
       safe on Linux/glibc. If a future platform breaks this, switch to
       userfaultfd or a snapshot+memcmp scheme. */
    mprotect(page_addr, (size_t)therm_page_size, PROT_READ | PROT_WRITE);
}

int js_arena_thermometer_enable(void)
{
    if (therm_enabled)
        return 0;
    if (!js_arena_base_lo || !js_arena_base_hi)
        return -1; /* freeze hasn't published the range yet */

    therm_page_size = sysconf(_SC_PAGESIZE);
    if (therm_page_size <= 0)
        return -1;

    size_t base_size = (size_t)(js_arena_base_hi - js_arena_base_lo);
    if ((uintptr_t)js_arena_base_lo & (uintptr_t)(therm_page_size - 1))
        return -1; /* base must be page-aligned (it is when mmap'd) */

    therm_base_pages = base_size / (size_t)therm_page_size;
    size_t bitmap_bytes = (therm_base_pages + 7) / 8;
    therm_dirty_bitmap = calloc(1, bitmap_bytes ? bitmap_bytes : 1);
    if (!therm_dirty_bitmap)
        return -1;
    therm_baseline = malloc(base_size);
    if (!therm_baseline) {
        free(therm_dirty_bitmap);
        therm_dirty_bitmap = NULL;
        return -1;
    }
    memcpy(therm_baseline, js_arena_base_lo, base_size);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = therm_sigsegv;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &therm_prev_sa) < 0) {
        free(therm_dirty_bitmap);
        therm_dirty_bitmap = NULL;
        return -1;
    }

    if (mprotect((void *)js_arena_base_lo, base_size, PROT_READ) < 0) {
        sigaction(SIGSEGV, &therm_prev_sa, NULL);
        free(therm_dirty_bitmap);
        therm_dirty_bitmap = NULL;
        return -1;
    }

    therm_writes = 0;
    therm_pages_dirty = 0;
    therm_enabled = 1;
    return 0;
}

void js_arena_thermometer_disable(void)
{
    if (!therm_enabled)
        return;
    size_t base_size = (size_t)(js_arena_base_hi - js_arena_base_lo);
    mprotect((void *)js_arena_base_lo, base_size, PROT_READ | PROT_WRITE);
    sigaction(SIGSEGV, &therm_prev_sa, NULL);
    free(therm_dirty_bitmap);
    therm_dirty_bitmap = NULL;
    free(therm_baseline);
    therm_baseline = NULL;
    therm_writes = 0;
    therm_pages_dirty = 0;
    therm_enabled = 0;
}

void js_arena_thermometer_reset(void)
{
    if (!therm_enabled)
        return;
    /* Re-protect the entire base region in one syscall (cheap for our
       sizes) and clear the bitmap. Refresh the baseline so the changed-
       byte counters reflect mutations during the *next* request, not
       cumulative drift. */
    size_t base_size = (size_t)(js_arena_base_hi - js_arena_base_lo);
    mprotect((void *)js_arena_base_lo, base_size, PROT_READ);
    if (therm_baseline)
        memcpy(therm_baseline, js_arena_base_lo, base_size);
    size_t bitmap_bytes = (therm_base_pages + 7) / 8;
    memset(therm_dirty_bitmap, 0, bitmap_bytes);
    therm_writes = 0;
    therm_pages_dirty = 0;
}

size_t js_arena_thermometer_pages(void)  { return therm_pages_dirty; }
size_t js_arena_thermometer_writes(void) { return therm_writes; }
size_t js_arena_thermometer_page_size(void) {
    return therm_enabled ? (size_t)therm_page_size : 0;
}

size_t js_arena_thermometer_dirty_offsets(size_t *out, size_t cap)
{
    if (!therm_enabled || !therm_dirty_bitmap)
        return 0;
    size_t found = 0;
    for (size_t i = 0; i < therm_base_pages; i++) {
        if (therm_dirty_bitmap[i >> 3] & (1u << (i & 7))) {
            if (found < cap)
                out[found] = i * (size_t)therm_page_size;
            found++;
        }
    }
    return found;
}

size_t js_arena_thermometer_changed_in_page(size_t page_offset)
{
    if (!therm_enabled || !therm_baseline || !js_arena_base_lo)
        return 0;
    size_t base_size = (size_t)(js_arena_base_hi - js_arena_base_lo);
    if (page_offset >= base_size)
        return 0;
    size_t end = page_offset + (size_t)therm_page_size;
    if (end > base_size)
        end = base_size;
    const uint8_t *live = js_arena_base_lo + page_offset;
    const uint8_t *base = therm_baseline    + page_offset;
    size_t n = 0;
    for (size_t i = 0, len = end - page_offset; i < len; i++)
        if (live[i] != base[i])
            n++;
    return n;
}

size_t js_arena_thermometer_changed_bytes(void)
{
    if (!therm_enabled || !therm_dirty_bitmap)
        return 0;
    size_t total = 0;
    for (size_t i = 0; i < therm_base_pages; i++) {
        if (therm_dirty_bitmap[i >> 3] & (1u << (i & 7)))
            total += js_arena_thermometer_changed_in_page(i * (size_t)therm_page_size);
    }
    return total;
}
