/*
 * Regression test: base-resident runtime tables must never be grown
 * (and thereby relocated into the request arena) after JS_FreezeRuntime.
 *
 * rove#735. Two branches in quickjs.c grow a base table on demand and
 * were not gated on rt->is_arena:
 *
 *   __JS_NewAtom      "if (rt->atom_free_index == 0) { ... }"  -- grows
 *                     rt->atom_array
 *   js_new_shape2     "if (2*(shape_hash_count+1) > size) { ... }" --
 *                     calls resize_shape_hash on rt->shape_hash
 *
 * Post-freeze, js_realloc_rt follows the *current* arena mode, so either
 * branch copies a base table into the REQUEST arena and writes the new
 * pointer back into the JSRuntime (itself base-resident). The next
 * JS_ResetRequestArena recycles that memory, and every later
 * js_atom_struct() read dereferences whatever the previous request left
 * behind -- e.g. js_empty_string() on the first JS_NewStringLen(ctx, s, 0)
 * of the next request, which is how this first surfaced.
 *
 * Neither branch is needed in arena mode: post-freeze interning
 * allocates from rt->req->atom_overlay and hashed request shapes link
 * into rt->req->shape_overlay, each with its own free list.
 *
 * Why a SWEEP and not a single case: the atom branch only fires when
 * rt->atom_free_index happens to be exactly 0 at freeze -- i.e. the
 * snapshot's atoms exactly filled the base array (sizes step 711, 1066,
 * 1599, 2398 ...). Adding or removing one atom anywhere in the snapshot
 * flips it. Sweeping the snapshot's atom count one at a time is the only
 * way to hit the boundary reliably; on the unfixed tree exactly 2 of 761
 * sweep points crash. That needle is also why arena-test262 never caught
 * it: the corpus never builds a snapshot that lands on the boundary.
 *
 * Detection is the thermometer rather than the crash: the offending
 * realloc writes rt->atom_array / rt->atom_size / rt->atom_free_index,
 * all in base, so the failure shows up as base pages dirtied during a
 * request -- reported before the process gets a chance to segfault on a
 * later reset. The functional checks after each reset (empty string,
 * base atom round-trip) are the backstop for builds with no thermometer.
 *
 * Build:  part of the arena_target foreach in CMakeLists.txt
 * Usage:  arena-atomgrow [pad_max]      (default 800)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "qjs-arena.h"

#define REQUESTS_PER_PAD 3

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

/* The exact shape of the reported crash: js_empty_string() reads
   rt->atom_array[JS_ATOM_empty_string]. Plus a round-trip through a
   base atom, which walks rt->atom_hash and dereferences the entries it
   finds. Both are silent no-ops on a healthy runtime and segfault on a
   relocated atom array. */
static int atom_table_intact(JSContext *ctx)
{
    int ok = 1;

    JSValue empty = JS_NewStringLen(ctx, "", 0);
    if (JS_VALUE_GET_TAG(empty) != JS_TAG_STRING) {
        fprintf(stderr, "  empty string is not a string\n");
        ok = 0;
    } else {
        const char *s = JS_ToCString(ctx, empty);
        if (!s || s[0] != '\0') {
            fprintf(stderr, "  empty string round-trip is not empty\n");
            ok = 0;
        }
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, empty);

    JSAtom a = JS_NewAtom(ctx, "toString");
    const char *s = JS_AtomToCString(ctx, a);
    if (!s || strcmp(s, "toString") != 0) {
        fprintf(stderr, "  base atom round-trip returned '%s'\n", s ? s : "(null)");
        ok = 0;
    }
    if (s) JS_FreeCString(ctx, s);
    JS_FreeAtom(ctx, a);

    return ok;
}

/* Build a snapshot carrying `pad` extra unique atoms, freeze it, and run
   REQUESTS_PER_PAD requests that each intern fresh atoms and fresh
   shapes. Returns base pages dirtied across those requests, or (size_t)-1
   on a setup/consistency failure. */
static size_t sweep_one(int pad, int therm)
{
    JSRuntime *rt = JS_NewRuntimeArena(6u << 20, 4u << 20);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return (size_t)-1; }
    JSDualArena *da = JS_GetDualArena(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return (size_t)-1; }

    /* `pad` unique property names -> `pad` unique atoms in the snapshot.
       One object literal per property so the snapshot also grows a fresh
       hashed shape per step, exercising the shape_hash branch alongside
       the atom-array one. */
    size_t cap = 4096 + (size_t)pad * 48;
    char *src = malloc(cap);
    if (!src) { fprintf(stderr, "malloc failed\n"); return (size_t)-1; }
    size_t n = snprintf(src, cap, "globalThis.pad = [];");
    for (int i = 0; i < pad; i++)
        n += snprintf(src + n, cap - n, "pad.push({p%06d:%d});", i, i);
    int bad = eval_ok(ctx, src, "snapshot");
    free(src);
    if (bad) return (size_t)-1;

    JS_FreezeRuntime(rt);
    js_dual_arena_set_request_mode(da, JS_ARENA_REQ_MODE_BUMP);

    size_t pages = 0;
    if (therm && js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n");
        return (size_t)-1;
    }

    for (int r = 0; r < REQUESTS_PER_PAD; r++) {
        JS_ResetRequestArena(rt);

        /* Read the base atom table before doing anything else: on the
           request after the corrupting one this is where it dies. */
        if (!atom_table_intact(ctx)) {
            fprintf(stderr, "  (pad=%d request=%d)\n", pad, r);
            return (size_t)-1;
        }

        if (therm) js_arena_thermometer_reset();

        /* Intern atoms and shapes that the snapshot has never seen, so
           the request drives __JS_NewAtom and js_new_shape2 into their
           respective grow branches if those are still reachable. */
        char req[256];
        snprintf(req, sizeof req,
                 "(() => { let o = {}, n = 0;"
                 " for (let i = 0; i < 64; i++) { o['r%d_k' + i] = i;"
                 " n += Object.keys({ ['r%d_s' + i]: i }).length; }"
                 " return n + Object.keys(o).length; })()", r, r);
        if (eval_ok(ctx, req, "request") < 0)
            return (size_t)-1;

        /* Report a base write the moment it happens: the corrupting
           realloc lands here, but the crash it causes lands on the NEXT
           reset, so accumulating and checking at the end would let the
           segfault beat the diagnosis. */
        if (therm) {
            size_t p = js_arena_thermometer_pages();
            if (p != 0) {
                fprintf(stderr, "  (pad=%d request=%d dirtied %zu base page(s))\n",
                        pad, r, p);
                fflush(stderr);
                pages += p;
                break;
            }
        }

        if (!atom_table_intact(ctx)) {
            fprintf(stderr, "  (pad=%d request=%d, after eval)\n", pad, r);
            return (size_t)-1;
        }
    }

    if (therm) js_arena_thermometer_disable();
    /* arena mode: skip JS_FreeContext/JS_FreeRuntime; free the arena. */
    js_dual_arena_free(da);
    return pages;
}

int main(int argc, char **argv)
{
    int pad_max = argc > 1 ? atoi(argv[1]) : 800;
    if (pad_max < 0) pad_max = 0;

    int therm = 1;
#if defined(__wasm__) || defined(ARENA_NO_THERM)
    therm = 0;
    printf("thermometer unavailable in this build; "
           "functional checks only\n");
#endif

    printf("sweeping snapshot atom count, pad = 0..%d "
           "(%d requests each)...\n", pad_max, REQUESTS_PER_PAD);

    int fail = 0, dirty_points = 0;
    for (int pad = 0; pad <= pad_max; pad++) {
        size_t pages = sweep_one(pad, therm);
        if (pages == (size_t)-1) {
            fprintf(stderr,
                "FAIL: pad=%d -- the base atom table did not survive a "
                "request reset.\n"
                "      A base table was relocated into the request arena "
                "post-freeze; see the header of this file.\n", pad);
            fail = 1;
            break;
        }
        if (pages != 0) {
            fprintf(stderr,
                "FAIL: pad=%d dirtied %zu base page(s) during a "
                "post-freeze request.\n"
                "      Base must be inviolate. The likely culprit is a "
                "grow/resize branch on a base table\n"
                "      (rt->atom_array, rt->shape_hash) that is not gated "
                "on rt->is_arena.\n", pad, pages);
            dirty_points++;
            fail = 1;
        }
        if ((pad % 100) == 0) {
            printf("  ... pad=%d clean\n", pad);
            fflush(stdout);
        }
    }

    fflush(stdout);
    if (fail) {
        if (dirty_points)
            fprintf(stderr, "%d sweep point(s) dirtied base.\n", dirty_points);
        return 1;
    }
    printf("PASS: %d snapshot sizes swept, base tables stayed in base "
           "across every reset.\n", pad_max + 1);
    return 0;
}
