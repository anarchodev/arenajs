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

    /* arena: skip JS_FreeContext / JS_FreeRuntime entirely. Their walks
       (assert(list_empty(&rt->gc_obj_list)), per-atom JS_FreeAtomStruct,
       etc.) are for the refcount-based default path. In arena mode the
       whole arena is reclaimed wholesale by js_dual_arena_free; the
       runtime, contexts, and everything they own live in arena pages
       that vanish in one munmap. */
    js_dual_arena_free(da);
    return 0;
}
