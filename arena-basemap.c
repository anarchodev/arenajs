/*
 * Regression test: iterating a base-resident Map/Set must not dirty base.
 *
 * A populated Map/Set built during snapshot setup has its JSMapRecords
 * allocated in the base arena. Read-only iteration at request time
 * (forEach, for..of, spread) bumps the iterator-lock field
 * JSMapRecord.ref_count (quickjs.c js_map_forEach / map iterator next),
 * which is NOT routed through arena_rc_inc / js_arena_ptr_is_base like
 * the object-header refcounts are. The lock is bumped and restored, so a
 * net-value memcmp reads zero, but the base *page* is still written and
 * faults under the CoW thermometer -- unique-pages-dirtied is the cost
 * metric that matters, so that is what we assert on.
 *
 * test262 cannot catch this: a bare frozen runtime has zero base map
 * records (JS_AddIntrinsicMapSet installs only prototypes; every
 * map_add_record caller is a runtime JS op), so no test that builds its
 * own collections ever iterates a *base* one. This test builds the base
 * collection explicitly.
 *
 * Expected: PASS (0 base pages dirtied) once the four mr->ref_count sites
 * are guarded with !js_arena_ptr_is_base(mr). Until then it FAILS RED,
 * which is the point -- it makes the latent hole visible.
 *
 * Build:  part of the arena_target foreach in CMakeLists.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "qjs-arena.h"

static int eval_ok(JSContext *ctx, const char *src, const char *label)
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
    JS_FreeValue(ctx, v);
    return 0;
}

/* Run one request in isolation: reset the request arena and the
 * thermometer, evaluate `src`, then report how many base pages it
 * dirtied. Returns the dirty-page count, or (size_t)-1 on JS exception. */
static size_t measure_request(JSRuntime *rt, JSContext *ctx,
                              const char *src, const char *label)
{
    JS_ResetRequestArena(rt);
    js_arena_thermometer_reset();
    if (eval_ok(ctx, src, label) < 0)
        return (size_t)-1;

    size_t pages  = js_arena_thermometer_pages();
    size_t writes = js_arena_thermometer_writes();
    printf("  [%-16s] base pages dirtied: %zu (writes=%zu, changed_bytes=%zu)\n",
           label, pages, writes, js_arena_thermometer_changed_bytes());

    if (pages > 0) {
        size_t offs[32];
        size_t n = js_arena_thermometer_dirty_offsets(offs, 32);
        printf("      dirty page offsets:");
        for (size_t i = 0; i < n && i < 32; i++)
            printf(" +%zu", offs[i]);
        printf("\n");
    }
    return pages;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntimeArena(8 * 1024 * 1024, 8 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 2; }

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 2; }

    /* --- snapshot: build populated base-resident collections --- */
    if (eval_ok(ctx,
        "globalThis.ROUTES = new Map();"
        "for (let i = 0; i < 64; i++) ROUTES.set('/route/' + i, i);"
        "globalThis.TAGS = new Set();"
        "for (let i = 0; i < 64; i++) TAGS.add('tag' + i);"
        "'snapshot ok'", "snapshot") < 0) return 2;

    JS_FreezeRuntime(rt);

    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 2;
    }

    /* Warm-up: a request that touches no base collection, to burn any
     * one-time post-freeze lazy paths so they aren't misattributed to
     * the map iteration below. Its own dirty count is informational. */
    printf("warm-up (no base-collection access):\n");
    measure_request(rt, ctx,
        "(() => { let a = []; for (let i = 0; i < 200; i++) a.push(i*i);"
        " return a.reduce((x,y)=>x+y, 0); })()", "warmup");

    /* Control: iterate a REQUEST-local map. Same code path, but the
     * records live in the request arena, so this must be 0 base pages
     * both before and after the fix -- it isolates "iteration" from
     * "iteration of a *base* collection". */
    printf("control (request-local collection):\n");
    size_t ctrl = measure_request(rt, ctx,
        "(() => { const m = new Map();"
        " for (let i = 0; i < 64; i++) m.set(i, i*i);"
        " let n = 0; m.forEach((v,k) => { n += v; });"
        " for (const [k,v] of m) n += k;"
        " return n; })()", "ctrl-local-map");

    /* The finding: read-only iteration of the BASE collections. */
    printf("subject (base-resident collection iteration):\n");
    size_t f_foreach = measure_request(rt, ctx,
        "(() => { let n = 0; ROUTES.forEach((v,k) => { n += v; }); return n; })()",
        "base-forEach");
    size_t f_forof = measure_request(rt, ctx,
        "(() => { let n = 0; for (const [k,v] of ROUTES) n += v; return n; })()",
        "base-forof");
    size_t f_spread = measure_request(rt, ctx,
        "(() => [...TAGS].length)()",
        "base-spread-set");
    size_t f_setforeach = measure_request(rt, ctx,
        "(() => { let n = 0; TAGS.forEach(v => { n += v.length; }); return n; })()",
        "base-set-forEach");

    /* Fail-loud floor: mutating a base collection must throw TypeError and
       must NOT dirty base (the guard rejects before any base write). The
       inner JS asserts each attempt throws; if one doesn't, it throws an
       Error, which surfaces as (size_t)-1 below. */
    printf("mutation guard (base collection must reject in place):\n");
    size_t f_mut = measure_request(rt, ctx,
        "(() => {"
        "  const mustThrow = (fn, what) => { let ok = false;"
        "    try { fn(); } catch (e) { ok = (e instanceof TypeError); }"
        "    if (!ok) throw new Error(what + ' did not throw TypeError'); };"
        "  mustThrow(() => ROUTES.set('k', 1), 'Map.set');"
        "  mustThrow(() => ROUTES.delete('/route/0'), 'Map.delete');"
        "  mustThrow(() => ROUTES.clear(), 'Map.clear');"
        "  mustThrow(() => TAGS.add('z'), 'Set.add');"
        "  mustThrow(() => TAGS.delete('tag0'), 'Set.delete');"
        "  mustThrow(() => TAGS.clear(), 'Set.clear');"
        "  return 0; })()", "base-mutate-throws");

    /* Sanctioned escape hatch: copy to a request-local collection, which
       is freely mutable and reads the base map without dirtying it. */
    printf("copy idiom (new Map(base) is mutable, base stays clean):\n");
    size_t f_copy = measure_request(rt, ctx,
        "(() => { const m = new Map(ROUTES); m.set('extra', 99);"
        " m.delete('/route/0'); return m.size; })()", "copy-mutate");

    js_arena_thermometer_disable();

    /* --- verdict --- */
    fflush(stdout);  /* keep the verdict (stderr) below the per-request data */
    int fail = 0;
    if (f_mut == (size_t)-1) {
        fprintf(stderr, "FAIL: an in-place base-collection mutation did NOT throw "
                        "TypeError -- the fail-loud guard is missing/incomplete.\n");
        fail = 1;
    } else if (f_mut != 0) {
        fprintf(stderr, "FAIL: a rejected base mutation still dirtied %zu base "
                        "page(s) -- the guard must reject before any base write.\n", f_mut);
        fail = 1;
    }
    if (f_copy == (size_t)-1) {
        fprintf(stderr, "FAIL: the new Map(base) copy path threw.\n");
        fail = 1;
    } else if (f_copy != 0) {
        fprintf(stderr, "FAIL: a request-local map copy dirtied %zu base page(s).\n", f_copy);
        fail = 1;
    }
    if (ctrl == (size_t)-1 || f_foreach == (size_t)-1 || f_forof == (size_t)-1 ||
        f_spread == (size_t)-1 || f_setforeach == (size_t)-1) {
        fprintf(stderr, "FAIL: a request threw\n");
        fail = 1;
    }
    if (ctrl != 0) {
        fprintf(stderr, "FAIL: control (request-local) iteration dirtied %zu base "
                        "page(s) -- test is not isolating correctly\n", ctrl);
        fail = 1;
    }
    size_t subject = f_foreach + f_forof + f_spread + f_setforeach;
    if (subject != 0) {
        fprintf(stderr,
            "FAIL: read-only iteration of base-resident Map/Set dirtied base "
            "(%zu total page-hits: forEach=%zu forof=%zu spread=%zu setForEach=%zu).\n"
            "      Base must be inviolate against any JS code. Guard the four "
            "mr->ref_count sites (js_map_forEach / map iterator next / "
            "map_delete_record / map_decref_record) with !js_arena_ptr_is_base(mr).\n",
            subject, f_foreach, f_forof, f_spread, f_setforeach);
        fail = 1;
    }

    if (!fail)
        printf("PASS: base-resident Map/Set iteration left base inviolate.\n");

    /* arena mode: skip JS_FreeContext/JS_FreeRuntime; wholesale-free the arena. */
    js_dual_arena_free(JS_GetDualArena(rt));
    return fail ? 1 : 0;
}
