/*
 * Regression test: mutating a base-resident object must not write base.
 *
 * The shadow mechanism (js_object_for_write) covers a base object's
 * NAMED PROPERTIES. Every other piece of per-object mutable state used
 * to be unprotected, because js_clone_jsobject_for_write shallow-copies
 * the `u` union and the mutating fast paths took the element pointer
 * straight off the base JSObject. For fast arrays that meant:
 *
 *   - in-place writes (index store, sort, reverse, fill, copyWithin,
 *     pop, shift, splice) went into the snapshot and were visible to
 *     EVERY LATER REQUEST — a cross-request data channel;
 *   - growth (push) reallocated u.array.u.values into the request arena
 *     and stored that pointer back into the base JSObject, so the next
 *     JS_ResetRequestArena left it dangling and the following request
 *     segfaulted. Same shape as rove#735, but reachable from ordinary
 *     JS with no knife-edge.
 *
 * The fix is copy-on-write: the shadow gets its own copy of the element
 * array (kept in fast-array form, so base arrays keep their compact
 * layout and their read speed), and the four mutating chokepoints
 * redirect to it — JS_SetPropertyValue, js_array_push, js_array_pop,
 * js_array_reverse, plus JS_CopySubArray for copyWithin/splice.
 *
 * This test asserts both halves, because they fail independently:
 *   1. thermometer: no base page is dirtied by any mutator;
 *   2. isolation: the next request sees the SNAPSHOT value, not the
 *      previous request's write.
 * A shadow that is created but still aliases base storage passes (2)
 * for the wrong reason and fails (1); a mutator that writes a private
 * copy of the wrong thing passes (1) and fails (2).
 *
 * Read-only fast paths (indexOf, includes, toSorted, slice) are
 * deliberately NOT redirected — shadowing those would copy the whole
 * element array on a pure read. The "read only" case below guards that:
 * it must stay at 0 pages without creating a shadow.
 *
 * Base typed arrays and ArrayBuffers take the other route: their bytes
 * belong to the ArrayBuffer, so copy-on-write would have to move every
 * aliasing view atomically, carry detached/resizable state along, and
 * memcpy the whole buffer per request — an unbounded, embedder-controlled
 * cost, i.e. a capacity cliff rather than a fix. They are marked
 * immutable at freeze instead, so writes are refused.
 *
 * SCOPE: fast arrays, typed arrays and ArrayBuffers. Base Date, Promise
 * and generators hold mutable state elsewhere and are handled separately;
 * they are not asserted here yet.
 *
 * Build:  part of the arena_target foreach in CMakeLists.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "quickjs.h"
#include "qjs-arena.h"

static int nfail;

static JSValue eval_val(JSContext *ctx, const char *src, const char *label)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), label, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        fprintf(stderr, "FAIL: [%s] threw: %s\n", label, s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, v);
        nfail++;
        return JS_UNDEFINED;
    }
    return v;
}

/* Run `src` as its own request; assert it dirtied no base page. */
static void mutates_no_base(JSRuntime *rt, JSContext *ctx,
                            const char *label, const char *src)
{
    JS_ResetRequestArena(rt);
    js_arena_thermometer_reset();
    JS_FreeValue(ctx, eval_val(ctx, src, label));
    size_t pages = js_arena_thermometer_pages();
    if (pages != 0) {
        fprintf(stderr,
            "FAIL: %-28s dirtied %zu base page(s), %zu byte(s).\n"
            "      A base array's element storage was written in place. The\n"
            "      mutator needs to route through js_object_for_write.\n",
            label, pages, js_arena_thermometer_changed_bytes());
        nfail++;
    } else {
        printf("  ok  %-28s 0 base pages\n", label);
    }
}

/* Mutate in one request, then assert the NEXT request sees the snapshot
   value — the mutation must not have escaped its request. */
static void isolated_across_reset(JSRuntime *rt, JSContext *ctx,
                                  const char *label,
                                  const char *mutate, const char *read,
                                  const char *expect)
{
    JS_ResetRequestArena(rt);
    JS_FreeValue(ctx, eval_val(ctx, mutate, label));

    JS_ResetRequestArena(rt);
    JSValue v = eval_val(ctx, read, label);
    const char *got = JS_ToCString(ctx, v);
    if (!got || strcmp(got, expect) != 0) {
        fprintf(stderr,
            "FAIL: %-28s next request saw '%s', expected '%s'.\n"
            "      A base mutation leaked across JS_ResetRequestArena;\n"
            "      request N's write is visible to request N+1.\n",
            label, got ? got : "(null)", expect);
        nfail++;
    } else {
        printf("  ok  %-28s next request sees '%s'\n", label, expect);
    }
    if (got) JS_FreeCString(ctx, got);
    JS_FreeValue(ctx, v);
}

/* Assert a mutation is REFUSED — throws — and dirties no base page.
   Base ArrayBuffers are marked immutable at freeze rather than
   copy-on-written: their bytes belong to the buffer, so a copy would
   have to move every aliasing view atomically and memcpy the whole
   buffer per request, an unbounded embedder-controlled cost. */
static void mutation_refused(JSRuntime *rt, JSContext *ctx,
                             const char *label, const char *src)
{
    JS_ResetRequestArena(rt);
    js_arena_thermometer_reset();
    JSValue v = JS_Eval(ctx, src, strlen(src), label, JS_EVAL_TYPE_GLOBAL);
    bool threw = JS_IsException(v);
    if (threw)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    size_t pages = js_arena_thermometer_pages();
    if (!threw) {
        fprintf(stderr, "FAIL: %-28s did NOT throw; a base typed-array "
                        "mutation must be refused.\n", label);
        nfail++;
    } else if (pages != 0) {
        fprintf(stderr, "FAIL: %-28s threw but still dirtied %zu base "
                        "page(s).\n", label, pages);
        nfail++;
    } else {
        printf("  ok  %-28s refused, 0 base pages\n", label);
    }
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntimeArena(16 * 1024 * 1024, 16 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 2; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 2; }

    /* One base array per mutator: they run in separate requests, but a
       shared target would let one case's shadow mask another's bug. */
    JSValue snap = eval_val(ctx,
        "globalThis.A_push=[1,2,3,4];  globalThis.A_store=[1,2,3,4];"
        "globalThis.A_sort=[3,1,4,2];  globalThis.A_rev=[1,2,3,4];"
        "globalThis.A_pop=[1,2,3,4];   globalThis.A_shift=[1,2,3,4];"
        "globalThis.A_splice=[1,2,3,4];globalThis.A_fill=[1,2,3,4];"
        "globalThis.A_copy=[1,2,3,4];  globalThis.A_defp=[1,2,3,4];"
        "globalThis.A_len=[1,2,3,4];   globalThis.A_read=[1,2,3,4];"
        "globalThis.A_iso=[1,2,3,4];   globalThis.A_grow=[1,2,3,4];"
        "globalThis.TA=new Uint8Array(8); globalThis.AB=new ArrayBuffer(8);"
        "globalThis.ABR=new ArrayBuffer(8,{maxByteLength:16});"
        "'ok'", "snapshot");
    JS_FreeValue(ctx, snap);
    if (nfail) return 2;

    JS_FreezeRuntime(rt);
    js_dual_arena_set_request_mode(JS_GetDualArena(rt), JS_ARENA_REQ_MODE_BUMP);
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 2;
    }

    printf("control (request-local arrays must be free of all this):\n");
    mutates_no_base(rt, ctx, "control: local push",
        "(()=>{const a=[1,2,3]; for(let i=0;i<64;i++)a.push(i); return a.length;})()");
    mutates_no_base(rt, ctx, "control: local store",
        "(()=>{const a=[1,2,3]; a[0]=9; return a[0];})()");

    printf("base fast-array mutators:\n");
    mutates_no_base(rt, ctx, "push (grows storage)", "A_push.push(99), A_push.length");
    mutates_no_base(rt, ctx, "index store",         "A_store[0]=42, A_store[0]");
    mutates_no_base(rt, ctx, "sort",                "A_sort.sort(), A_sort.join(',')");
    mutates_no_base(rt, ctx, "reverse",             "A_rev.reverse(), A_rev.join(',')");
    mutates_no_base(rt, ctx, "pop",                 "A_pop.pop(), A_pop.length");
    mutates_no_base(rt, ctx, "shift",               "A_shift.shift(), A_shift.length");
    mutates_no_base(rt, ctx, "splice",              "A_splice.splice(1,1), A_splice.length");
    mutates_no_base(rt, ctx, "fill",                "A_fill.fill(0), A_fill.join(',')");
    mutates_no_base(rt, ctx, "copyWithin",          "A_copy.copyWithin(0,1), A_copy.join(',')");
    mutates_no_base(rt, ctx, "defineProperty index",
        "Object.defineProperty(A_defp,'0',{value:9}), A_defp[0]");
    mutates_no_base(rt, ctx, "length truncate",     "A_len.length=0, A_len.length");
    mutates_no_base(rt, ctx, "read only (no shadow)", "A_read.join(',')");

    printf("isolation across reset:\n");
    isolated_across_reset(rt, ctx, "in-place write",
        "A_iso[0]=100, A_iso.join(',')", "A_iso.join(',')", "1,2,3,4");
    isolated_across_reset(rt, ctx, "growth then reset",
        "A_grow.push(99), A_grow.join(',')", "A_grow.join(',')", "1,2,3,4");

    printf("base typed arrays / ArrayBuffers (refused, not copy-on-written):\n");
    /* Element stores are refused by upstream's immutable-ArrayBuffer
       path, which returns false rather than throwing, so they are
       asserted for effect and isolation rather than for a throw. */
    mutates_no_base(rt, ctx, "TA element store",   "TA[0]=7, TA[0]");
    mutates_no_base(rt, ctx, "view onto base buffer",
        "(()=>{const v=new Uint8Array(AB); return v.length;})()");
    mutates_no_base(rt, ctx, "TA read",            "TA.join(',')");
    mutation_refused(rt, ctx, "TA.fill",           "TA.fill(9)");
    mutation_refused(rt, ctx, "TA.sort",           "TA.sort()");
    mutation_refused(rt, ctx, "TA.reverse",        "TA.reverse()");
    mutation_refused(rt, ctx, "TA.set",            "TA.set([1,2])");
    mutation_refused(rt, ctx, "TA.copyWithin",     "TA.copyWithin(0,1)");
    mutation_refused(rt, ctx, "DataView set",
        "new DataView(AB).setUint8(0,5)");
    mutation_refused(rt, ctx, "ArrayBuffer.transfer", "AB.transfer()");
    mutation_refused(rt, ctx, "ArrayBuffer.resize",   "ABR.resize(16)");
    isolated_across_reset(rt, ctx, "TA store isolation",
        "TA[0]=200, 1", "TA.join(',')", "0,0,0,0,0,0,0,0");
    /* The sanctioned escape hatch must still work. */
    mutates_no_base(rt, ctx, "copy of base TA is writable",
        "(()=>{const c=new Uint8Array(TA); c[0]=5; return c[0];})()");

    /* The growth case used to leave u.array.u.values pointing into the
       request arena. Churn the arena hard, then read through it: on the
       old code this is where the segfault landed. */
    printf("post-growth churn (this is where the old code segfaulted):\n");
    JS_ResetRequestArena(rt);
    JS_FreeValue(ctx, eval_val(ctx, "A_grow.push(1,2,3,4,5,6,7,8)", "grow"));
    JS_ResetRequestArena(rt);
    JS_FreeValue(ctx, eval_val(ctx,
        "(()=>{const s=[]; for(let i=0;i<20000;i++) s.push({q:i}); return s.length;})()",
        "churn"));
    {
        JSValue v = eval_val(ctx, "A_grow.join(',')", "read-after-churn");
        const char *got = JS_ToCString(ctx, v);
        if (!got || strcmp(got, "1,2,3,4") != 0) {
            fprintf(stderr,
                "FAIL: base array read '%s' after churn, expected '1,2,3,4' --\n"
                "      its element storage was relocated into the request arena.\n",
                got ? got : "(null)");
            nfail++;
        } else {
            printf("  ok  %-28s survived, reads '1,2,3,4'\n", "read after churn");
        }
        if (got) JS_FreeCString(ctx, got);
        JS_FreeValue(ctx, v);
    }

    js_arena_thermometer_disable();
    fflush(stdout);

    if (nfail) {
        fprintf(stderr, "\n%d case(s) failed.\n", nfail);
        return 1;
    }
    printf("\nPASS: base fast arrays are copy-on-write and base typed arrays "
           "are immutable —\n      no base writes, no cross-request leakage.\n");
    js_dual_arena_free(JS_GetDualArena(rt));
    return 0;
}
