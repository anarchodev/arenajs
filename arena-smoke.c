/*
 * Smoke test for the dual-arena allocator.
 * Build:  cc arena-smoke.c -I. build/libqjs.a -lm -lpthread -o build/arena-smoke
 */
#include <signal.h>   /* SIGSEGV -- glibc leaks it via sys/wait.h, FreeBSD does not */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "quickjs.h"
#include "qjs-arena.h"

static int eval_print(JSContext *ctx, const char *src, const char *label)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), label, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        fprintf(stderr, "[%s] EXCEPTION: %s\n", label, s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, v);
        return -1;
    }
    const char *s = JS_ToCString(ctx, v);
    printf("[%s] => %s\n", label, s ? s : "(null)");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return 0;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntimeArena(4 * 1024 * 1024, 4 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 1; }

    JSDualArena *da = JS_GetDualArena(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 1; }

    /* --- snapshot mode --- */
    printf("frozen=%d  base_used=%zu  request_used=%zu\n",
           js_dual_arena_is_frozen(da),
           js_dual_arena_base_used(da),
           js_dual_arena_request_used(da));

    if (eval_print(ctx, "globalThis.GREET = name => `hello, ${name}!`; 'snapshot ok'",
                   "snapshot")) return 1;
    /* Snapshot collections: readable/iterable forever, immutable after
       freeze (mutators throw). Built here so the records live in base. */
    if (eval_print(ctx,
        "globalThis.BASE_MAP = new Map([['a',1],['b',2],['c',3]]);"
        "globalThis.BASE_SET = new Set(['x','y','z']);"
        "'collections ok'", "snapshot-collections")) return 1;

    size_t base_after_snapshot = js_dual_arena_base_used(da);
    size_t req_after_snapshot  = js_dual_arena_request_used(da);
    printf("after snapshot: base_used=%zu  request_used=%zu\n",
           base_after_snapshot, req_after_snapshot);

    /* --- freeze, then per-request work --- */
    JS_FreezeRuntime(rt);
    printf("frozen=%d\n", js_dual_arena_is_frozen(da));

    /* Request-chunk membership: a post-freeze allocation is request
       memory owned by this arena; the runtime struct (base) and a libc
       pointer are not. Both must hold or js_malloc_usable_size
       dispatches to the wrong size function. */
    {
        void *req_p = js_malloc_rt(rt, 64);
        void *libc_p = malloc(64);
        if (js_arena_ptr_request_owner(req_p) != js_dual_arena_current_request(da) ||
            js_arena_ptr_is_request(rt) ||
            js_arena_ptr_is_request(libc_p) ||
            !js_arena_ptr_is_base(rt)) {
            fprintf(stderr, "request chunk membership wrong\n"); return 1;
        }
        printf("chunk_tab: %u chunks registered (%zu B request)\n",
               js_arena_chunk_count, js_dual_arena_request_used(da));
        free(libc_p);
        js_free_rt(rt, req_p);
    }

    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 1;
    }
    {
        printf("--- JSRuntime layout ---\n");
        JS_DumpRuntimeOffsets(rt, stdout);
        printf("  ctx          = %p\n", (void *)ctx);
        printf("------------------------\n");
    }
    js_arena_thermometer_reset();

    if (eval_print(ctx,
        "let xs = []; for (let i = 0; i < 1000; i++) xs.push(i*i); "
        "GREET('arena') + ' sum=' + xs.reduce((a,b)=>a+b,0)",
        "request#1")) return 1;

    size_t base_after_req1 = js_dual_arena_base_used(da);
    size_t req_after_req1  = js_dual_arena_request_used(da);
    size_t pages_req1  = js_arena_thermometer_pages();
    size_t writes_req1 = js_arena_thermometer_writes();
    printf("after request#1: base_used=%zu (delta=%zu)  request_used=%zu (delta=%zu)\n",
           base_after_req1, base_after_req1 - base_after_snapshot,
           req_after_req1,  req_after_req1  - req_after_snapshot);
    printf("  thermometer: %zu base pages dirtied, %zu writes\n",
           pages_req1, writes_req1);
    {
        size_t offs[64];
        size_t n = js_arena_thermometer_dirty_offsets(offs, 64);
        size_t ps = js_arena_thermometer_page_size();
        printf("  page_size=%zu, %zu changed bytes total\n",
               ps, js_arena_thermometer_changed_bytes());
        for (size_t i = 0; i < n && i < 64; i++) {
            size_t bytes_chg = js_arena_thermometer_changed_in_page(offs[i]);
            printf("    +%-6zu  %5zu B changed", offs[i], bytes_chg);
            size_t boffs[64];
            size_t bn = js_arena_thermometer_changed_byte_offsets(offs[i], boffs, 64);
            if (bn > 0) {
                printf("  at:");
                for (size_t j = 0; j < bn && j < 64; j++)
                    printf(" %zu", boffs[j]);
                if (bn > 64) printf(" ... +%zu more", bn - 64);
            }
            printf("\n");
        }
    }

    js_arena_thermometer_reset();

    /* second request: expect more growth in request arena */
    if (eval_print(ctx,
        "let ys = []; for (let i = 0; i < 500; i++) ys.push({i, sq: i*i}); "
        "ys.length",
        "request#2")) return 1;

    size_t pages_req2  = js_arena_thermometer_pages();
    size_t writes_req2 = js_arena_thermometer_writes();
    printf("after request#2: base_used=%zu  request_used=%zu\n",
           js_dual_arena_base_used(da),
           js_dual_arena_request_used(da));
    printf("  thermometer: %zu base pages dirtied, %zu writes\n",
           pages_req2, writes_req2);
    {
        size_t offs[64];
        size_t n = js_arena_thermometer_dirty_offsets(offs, 64);
        printf("  %zu changed bytes total\n",
               js_arena_thermometer_changed_bytes());
        for (size_t i = 0; i < n && i < 64; i++) {
            size_t bytes_chg = js_arena_thermometer_changed_in_page(offs[i]);
            printf("    +%-6zu  %5zu B changed", offs[i], bytes_chg);
            size_t boffs[64];
            size_t bn = js_arena_thermometer_changed_byte_offsets(offs[i], boffs, 64);
            if (bn > 0) {
                printf("  at:");
                for (size_t j = 0; j < bn && j < 64; j++)
                    printf(" %zu", boffs[j]);
                if (bn > 64) printf(" ... +%zu more", bn - 64);
            }
            printf("\n");
        }
    }

    /* Disable before the determinism checks — they call setters that
       legitimately mutate ctx fields living in base, and we don't want
       to count those. */
    js_arena_thermometer_disable();

    /* --- determinism patch checks --- */
    /* Math.random() must return 0 until JS_SetRandomSeed is called
       (xorshift64 with zero state is identically zero). */
    if (eval_print(ctx, "Math.random()", "rand-unseeded")) return 1;
    /* performance.timeOrigin must be 0 until JS_SetTimeOrigin is called. */
    if (eval_print(ctx, "performance.timeOrigin", "timeOrigin-unset")) return 1;

    JS_SetRandomSeed(ctx, 0xdeadbeefcafef00dULL);
    JS_SetTimeOrigin(ctx, 12345.5);

    if (eval_print(ctx, "Math.random()", "rand-seeded")) return 1;
    if (eval_print(ctx, "performance.timeOrigin", "timeOrigin-set")) return 1;
    /* now() should be huge-positive (hrtime - 12345.5) since hrtime is
       monotonic since boot. */
    if (eval_print(ctx, "performance.now() > 0", "now-positive")) return 1;

    /* Snapshot collections: iteration and reads must not write base
       (the record-lock refcounts are skipped for base records), and
       every mutator must throw. */
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 1;
    }
    js_arena_thermometer_reset();
    if (eval_print(ctx,
        "let acc = [];"
        "for (const [k,v] of BASE_MAP) acc.push(k + v);"
        "BASE_MAP.forEach((v,k) => acc.push(k));"
        "acc.push(...BASE_SET);"
        "acc.push(BASE_MAP.get('b'), BASE_MAP.has('c'), BASE_MAP.size);"
        "const copy = new Map(BASE_MAP); copy.set('d', 4);"
        "acc.push(copy.size);"
        "acc.join(',')", "base-map-read")) return 1;
    if (eval_print(ctx,
        "const throws = [];"
        "for (const f of [() => BASE_MAP.set('z',9), () => BASE_MAP.delete('a'),"
        "                 () => BASE_MAP.clear(),   () => BASE_SET.add('w'),"
        "                 () => BASE_SET.delete('x'),"
        "                 () => BASE_MAP.getOrInsert && BASE_MAP.getOrInsert('q', 1)]) {"
        "  if (!f) continue;"
        "  try { f(); throws.push('NO-THROW'); }"
        "  catch (e) { throws.push(e instanceof TypeError ? 'T' : 'E'); }"
        "}"
        "throws.join('')", "base-map-mutate")) return 1;
    {
        size_t map_pages = js_arena_thermometer_pages();
        printf("snapshot-collection base pages dirtied: %zu (expect 0)\n",
               map_pages);
        if (map_pages != 0) return 1;
    }
    js_arena_thermometer_disable();

    /* arena: the determinism pins must land in JSRequestState, not
       base — regression check for the date/timeOrigin relocation.
       Re-arm the thermometer, re-pin everything, read the values back
       from JS, and require zero base pages dirtied. */
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer re-enable failed\n"); return 1;
    }
    js_arena_thermometer_reset();
    JS_SetRandomSeed(ctx, 0x1234567890abcdefULL);
    JS_SetTimeOrigin(ctx, 777.25);
    JS_SetDateNow(ctx, 1704067200000LL);
    if (eval_print(ctx, "Date.now()", "date-pinned")) return 1;
    if (eval_print(ctx, "performance.timeOrigin", "timeOrigin-repinned")) return 1;
    {
        size_t pin_pages = js_arena_thermometer_pages();
        printf("determinism-pin base pages dirtied: %zu (expect 0)\n", pin_pages);
        if (pin_pages != 0) return 1;
    }
    js_arena_thermometer_disable();

    /* --- hard mprotect: the MMU enforces inviolate-base --- */
    if (js_dual_arena_harden(da) != 0) {
        fprintf(stderr, "harden failed\n"); return 1;
    }
    /* A full request runs under enforcement: base reads, shadowed
       base-prototype writes, snapshot-collection iteration, pinned
       clock — every write lands request-side or the process dies. */
    JS_ResetRequestArena(rt);
    JS_SetDateNow(ctx, 777);
    if (eval_print(ctx,
        "globalThis.h1 = 'x'; Map.prototype.hardened = 1;"
        "[...BASE_MAP.keys()].join('') + ':' + Date.now()",
        "hardened-request")) return 1;
    /* And a genuine base write must die by SIGSEGV — prove it in a
       forked child (expect one [arena-harden] diagnostic below). */
    {
        pid_t pid = fork();
        if (pid == 0) {
            volatile uint8_t *pb = (volatile uint8_t *)rt; /* rt is base */
            *pb = *pb;      /* same-value write: the MMU faults anyway */
            _exit(0);       /* reached only if enforcement failed */
        }
        int st = 0;
        waitpid(pid, &st, 0);
        int killed = WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
        printf("hardened base write in child: %s\n",
               killed ? "SIGSEGV as expected" : "NOT ENFORCED");
        if (!killed) return 1;
    }
    if (js_dual_arena_unharden(da) != 0) {
        fprintf(stderr, "unharden failed\n"); return 1;
    }

    /* --- per-reset allocator mode selection --- */
    /* Bump mode: master semantics — cumulative usage, free is a no-op. */
    js_dual_arena_set_request_mode(da, JS_ARENA_REQ_MODE_BUMP);
    JS_ResetRequestArena(rt);
    if (js_dual_arena_request_mode(da) != JS_ARENA_REQ_MODE_BUMP) {
        fprintf(stderr, "mode getter mismatch\n"); return 1;
    }
    if (eval_print(ctx, "GREET('bump mode')", "mode-bump")) return 1;
    size_t bump_used = js_dual_arena_request_used(da);
    /* Back to GC mode: live-byte semantics return. */
    js_dual_arena_set_request_mode(da, JS_ARENA_REQ_MODE_GC);
    JS_ResetRequestArena(rt);
    if (eval_print(ctx, "GREET('gc mode')", "mode-gc")) return 1;
    printf("mode flip: bump used=%zu (cumulative), gc live=%zu, post-reset=%s\n",
           bump_used, js_dual_arena_request_used(da),
           js_dual_arena_request_mode(da) == JS_ARENA_REQ_MODE_GC ? "GC" : "BUMP");
    if (bump_used == 0) { fprintf(stderr, "bump accounting broken\n"); return 1; }

    /* --- request-region growth across provider extents ---
       The head extent is JS_ARENA_REQ_EXTENT_DEFAULT (256 KiB); a request
       that outgrows it pulls more from the provider (through MORECORE in
       GC mode, directly on the bump path) and reset hands them back. */
    {
        const char *grow =
            "var big = []; for (let i = 0; i < 8000; i++) "
            "big.push({ i, s: 'item-' + i }); big.length";
        size_t head_held = js_dual_arena_request_held(da);
        if (js_dual_arena_request_extents(da) != 1) {
            fprintf(stderr, "expected 1 extent after reset\n"); return 1;
        }
        /* GC mode */
        if (eval_print(ctx, grow, "grow-gc")) return 1;
        size_t ext_gc = js_dual_arena_request_extents(da);
        size_t held_gc = js_dual_arena_request_held(da);
        printf("grow-gc: extents=%zu held=%zu live=%zu\n",
               ext_gc, held_gc, js_dual_arena_request_used(da));
        if (ext_gc < 2 || held_gc <= head_held) {
            fprintf(stderr, "GC-mode request did not grow past the head\n"); return 1;
        }
        /* One allocation bigger than the policy extent gets its own. */
        if (eval_print(ctx, "new ArrayBuffer(1 << 20).byteLength", "grow-big")) return 1;
        if (js_dual_arena_request_extents(da) != ext_gc + 1 ||
            js_dual_arena_request_held(da) < held_gc + (1 << 20)) {
            fprintf(stderr, "oversize allocation did not get its own extent\n"); return 1;
        }
        JS_ResetRequestArena(rt);
        if (js_dual_arena_request_extents(da) != 1 ||
            js_dual_arena_request_held(da) != head_held ||
            js_dual_arena_request_used(da) != 0) {
            fprintf(stderr, "reset did not release extents\n"); return 1;
        }
        /* Bump mode */
        js_dual_arena_set_request_mode(da, JS_ARENA_REQ_MODE_BUMP);
        JS_ResetRequestArena(rt);
        if (eval_print(ctx, grow, "grow-bump")) return 1;
        printf("grow-bump: extents=%zu held=%zu cumulative=%zu\n",
               js_dual_arena_request_extents(da), js_dual_arena_request_held(da),
               js_dual_arena_request_used(da));
        if (js_dual_arena_request_extents(da) < 2) {
            fprintf(stderr, "bump-mode request did not grow past the head\n"); return 1;
        }
        js_dual_arena_set_request_mode(da, JS_ARENA_REQ_MODE_GC);
        JS_ResetRequestArena(rt);
        if (js_dual_arena_request_extents(da) != 1) {
            fprintf(stderr, "bump reset did not release extents\n"); return 1;
        }
        /* Budget: request_cap (4 MiB here) bounds what a request may hold.
           A single allocation that cannot fit is refused, throws, and
           latches the OOM record with the budget as the limit. */
        JSValue v = JS_Eval(ctx, "new ArrayBuffer(8 << 20)", 23, "oom",
                            JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(v)) {
            fprintf(stderr, "8 MiB allocation should exceed the 4 MiB budget\n");
            return 1;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        if (!js_dual_arena_oom_hit(da) ||
            js_dual_arena_oom_limit(da) != 4 * 1024 * 1024 - 16 - JS_ARENA_REQUEST_SLOT_SIZE) {
            fprintf(stderr, "OOM record wrong: hit=%d limit=%zu\n",
                    js_dual_arena_oom_hit(da), js_dual_arena_oom_limit(da));
            return 1;
        }
        printf("budget: refused %zu B at %zu live (limit %zu), extents=%zu\n",
               js_dual_arena_oom_requested(da), js_dual_arena_oom_used(da),
               js_dual_arena_oom_limit(da), js_dual_arena_request_extents(da));
        JS_ResetRequestArena(rt);
        if (js_dual_arena_oom_hit(da)) {
            fprintf(stderr, "reset did not clear the OOM latch\n"); return 1;
        }
        /* The runtime is still usable after a refused allocation. */
        if (eval_print(ctx, "GREET('after oom')", "post-oom")) return 1;
    }

    /* --- two request arenas on one runtime ---
       A request parks (its arena stays allocated, its JSRequestState
       stays in its head slot) while another runs; switching back finds
       its globals, shadows and objects intact. Selecting an arena
       rewrites the dual arena's heap cell, never the JSRuntime — the
       thermometer must see zero base pages across the whole dance. */
    {
        JSRequestArena *ra1 = js_dual_arena_current_request(da);
        JSRequestArena *ra2 = js_request_arena_new(da, 4 * 1024 * 1024, NULL);
        if (!ra2) { fprintf(stderr, "js_request_arena_new failed\n"); return 1; }
        if (js_arena_thermometer_enable() < 0) {
            fprintf(stderr, "thermometer enable failed\n"); return 1;
        }
        js_arena_thermometer_reset();
        /* request 1: fresh, leaves state behind and parks */
        JS_ResetRequestArena(rt);
        if (eval_print(ctx, "globalThis.who = 'one'; var keep = [1,2,3]; who",
                       "ra1-start")) return 1;
        /* request 2: fresh arena, fresh state — sees none of request 1 */
        js_dual_arena_select_request(da, ra2);
        JS_RelocateReqState(rt);
        if (eval_print(ctx, "typeof who + '/' + typeof keep", "ra2-fresh")) return 1;
        if (eval_print(ctx, "globalThis.who = 'two'; var big = new Array(50000).fill(1); who",
                       "ra2-run")) return 1;
        size_t ra2_ext = js_request_arena_extents(ra2);
        /* back to request 1: parked state intact, request 2's invisible */
        js_dual_arena_select_request(da, ra1);
        if (eval_print(ctx, "who + keep.length + '/' + typeof big", "ra1-resume")) return 1;
        /* and request 2 again, still holding its extents */
        js_dual_arena_select_request(da, ra2);
        if (eval_print(ctx, "who + big.length", "ra2-resume")) return 1;
        if (js_request_arena_extents(ra2) != ra2_ext || ra2_ext < 2) {
            fprintf(stderr, "parked arena did not keep its extents (%zu -> %zu)\n",
                    ra2_ext, js_request_arena_extents(ra2)); return 1;
        }
        printf("two requests: ra1 held=%zu ra2 held=%zu extents=%zu\n",
               js_request_arena_held(ra1), js_request_arena_held(ra2), ra2_ext);
        /* free the parked-then-finished request 2; request 1 continues */
        js_dual_arena_select_request(da, ra1);
        js_request_arena_free(ra2);
        if (eval_print(ctx, "who + keep.length", "ra1-after-free")) return 1;
        size_t two_pages = js_arena_thermometer_pages();
        js_arena_thermometer_disable();
        printf("  thermometer across request switches: %zu base pages dirtied\n",
               two_pages);
        if (two_pages != 0) {
            fprintf(stderr, "request switching wrote base\n"); return 1;
        }
    }

    /* arena: skip JS_FreeContext / JS_FreeRuntime entirely. Their walks
       (assert(list_empty(&rt->gc_obj_list)), per-atom JS_FreeAtomStruct,
       etc.) are for the refcount-based default path. In arena mode the
       whole arena is reclaimed wholesale by js_dual_arena_free; the
       runtime, contexts, and everything they own live in arena pages
       that vanish in one munmap. */
    js_dual_arena_free(da);
    if (js_arena_chunk_tab != NULL || js_arena_chunk_count != 0) {
        fprintf(stderr, "request chunk table not released\n"); return 1;
    }
    return 0;
}
