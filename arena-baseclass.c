/*
 * Exhaustive base-object mutation matrix, driven off the class table.
 *
 * Every other base-write bug we have found was found by someone
 * thinking of a case. That does not scale and it does not converge:
 * the fast-array hole sat behind a line in ARENA_PLAN.md for months,
 * and hand-enumerating the affected classes afterwards still missed
 * MappedArguments, the iterators and FinalizationRegistry.
 *
 * So this test enumerates the ENGINE's class table instead of a list
 * someone wrote down. For each registered class it either
 *
 *   MUTATE      - builds one in the snapshot and mutates it, asserting
 *                 (a) no base page is dirtied and (b) the next request
 *                 sees the snapshot value, or
 *   NO_STATE    - records why the class has no mutable state beyond
 *                 named properties (which the shadow already covers),
 *   UNREACHABLE - records why a JS snapshot cannot hold one, or
 *   COVERED     - defers to the harness that already asserts it, or
 *   REFUSED     - cannot be isolated per request, so JS_FreezeRuntime
 *                 refuses a snapshot containing one, or
 *   KNOWN_GAP   - a hole this test found that is not fixed yet.
 *
 * REFUSED entries are asserted in their own throwaway runtime via
 * JS_ScanSnapshotHazards, and deliberately NOT built into this test's
 * snapshot — doing that would abort the test itself, which is the
 * refusal working correctly and useless as a test.
 *
 * KNOWN_GAP is an expected failure WITH TEETH: the class is asserted to
 * still be broken. Fix one and this test fails, telling you to promote
 * the entry. A test that merely tolerates red entries is a test that
 * passes; this one refuses to let a fix go unrecorded, and the table
 * doubles as the backlog.
 *
 * The point is the failure mode when someone adds a class to the
 * engine: the id is registered, the table has no entry, and this test
 * FAILS until a human classifies it. It also pins each id to the name
 * JS_GetClassName reports, so an upstream enum reshuffle is caught
 * loudly instead of silently testing the wrong class.
 *
 * Isolation is asserted with a probe read at request entry. For
 * iterators that is the sharpest possible check: a base-resident
 * iterator whose cursor leaks returns the SECOND element on the second
 * request.
 *
 * Build:  part of the arena_target foreach in CMakeLists.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "quickjs.h"
#include "qjs-arena.h"

#define MAX_MUT 4

typedef enum {
    MUTATE,       /* must be clean */
    KNOWN_GAP,    /* known broken; asserted to still be broken, see below */
    REFUSED,      /* JS_FreezeRuntime refuses a snapshot holding one */
    NO_STATE,
    UNREACHABLE,
    COVERED,
} ClsKind;

typedef struct {
    unsigned    id;
    const char *enum_name;   /* JS_CLASS_* — for humans reading failures */
    const char *class_name;  /* what JS_GetClassName must report */
    ClsKind     kind;
    const char *note;        /* reason for NO_STATE / UNREACHABLE / COVERED */
    const char *ctor;        /* expression evaluated into globalThis.T */
    const char *mut[MAX_MUT];/* each runs as its own request */
    const char *probe;       /* read at request entry */
    const char *expect;      /* ... must equal this, every request */
} ClsEntry;

/* Keyed by class id. Order follows the JS_CLASS_* enum. */
static const ClsEntry TABLE[] = {
{1,"JS_CLASS_OBJECT","Object",MUTATE,NULL,"({a:1})",
    {"T.a=9","T.b=2","delete T.a",NULL},"T.a","1"},
{2,"JS_CLASS_ARRAY","Array",COVERED,"arena-baseobj: fast-array copy-on-write",NULL,{NULL},NULL,NULL},
{3,"JS_CLASS_ERROR","Error",MUTATE,NULL,"new Error('m')",
    {"T.message='x'","T.code=1",NULL},"T.message","m"},
{4,"JS_CLASS_NUMBER","Number",MUTATE,NULL,"new Number(7)",
    {"T.tag=1",NULL},"T.valueOf()","7"},
{5,"JS_CLASS_STRING","String",MUTATE,NULL,"new String('ab')",
    {"T.tag=1",NULL},"T.valueOf()","ab"},
{6,"JS_CLASS_BOOLEAN","Boolean",MUTATE,NULL,"new Boolean(true)",
    {"T.tag=1",NULL},"T.valueOf()","true"},
{7,"JS_CLASS_SYMBOL","Symbol",MUTATE,NULL,"Object(Symbol('s'))",
    {"T.tag=1",NULL},"typeof T.valueOf()","symbol"},
{8,"JS_CLASS_ARGUMENTS","Arguments",MUTATE,NULL,
    "(function(){'use strict'; return arguments;})(1,2,3)",
    {"T[0]=9","T.extra=1",NULL},"T[0]","1"},
{9,"JS_CLASS_MAPPED_ARGUMENTS","Arguments",MUTATE,NULL,
    "(function(a,b){ return arguments; })(1,2)",
    {"T[0]=9",NULL},"T[0]","1"},
{10,"JS_CLASS_DATE","Date",MUTATE,NULL,"new Date(0)",
    {"T.setTime(5)","T.setFullYear(1999)",NULL},"T.getTime()","0"},
{11,"JS_CLASS_MODULE_NS","Object",NO_STATE,
    "module namespace is spec-immutable: [[Set]] always fails",NULL,{NULL},NULL,NULL},
{12,"JS_CLASS_C_FUNCTION","Function",MUTATE,NULL,"Math.max",
    {"T.tag=1",NULL},"typeof T","function"},
{13,"JS_CLASS_BYTECODE_FUNCTION","Function",MUTATE,NULL,
    "(function f(){ return 1; })",
    {"T.tag=1","T.prototype.m=1",NULL},"T()","1"},
{14,"JS_CLASS_BOUND_FUNCTION","Function",MUTATE,NULL,
    "(function(){ return 2; }).bind(null)",
    {"T.tag=1",NULL},"T()","2"},
{15,"JS_CLASS_C_FUNCTION_DATA","Function",MUTATE,NULL,
    "Proxy.revocable({},{}).revoke",
    {"T.tag=1",NULL},"typeof T","function"},
{16,"JS_CLASS_C_CLOSURE","Function",UNREACHABLE,
    "only JS_NewCClosure creates one; no JS syntax reaches it",NULL,{NULL},NULL,NULL},
{17,"JS_CLASS_GENERATOR_FUNCTION","GeneratorFunction",MUTATE,NULL,
    "(function*(){ yield 1; })",
    {"T.tag=1",NULL},"typeof T","function"},
{18,"JS_CLASS_FOR_IN_ITERATOR","ForInIterator",UNREACHABLE,
    "internal to the for-in opcode; never escapes to JS",NULL,{NULL},NULL,NULL},
{19,"JS_CLASS_REGEXP","RegExp",MUTATE,NULL,"/a/g",
    {"T.exec('aaa')","T.lastIndex=5","T.tag=1",NULL},"T.lastIndex","0"},
{20,"JS_CLASS_ARRAY_BUFFER","ArrayBuffer",COVERED,"arena-baseobj: immutable at freeze",NULL,{NULL},NULL,NULL},
{21,"JS_CLASS_SHARED_ARRAY_BUFFER","SharedArrayBuffer",COVERED,"arena-baseobj: immutable at freeze",NULL,{NULL},NULL,NULL},
{22,"JS_CLASS_UINT8C_ARRAY","Uint8ClampedArray",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{23,"JS_CLASS_INT8_ARRAY","Int8Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{24,"JS_CLASS_UINT8_ARRAY","Uint8Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{25,"JS_CLASS_INT16_ARRAY","Int16Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{26,"JS_CLASS_UINT16_ARRAY","Uint16Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{27,"JS_CLASS_INT32_ARRAY","Int32Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{28,"JS_CLASS_UINT32_ARRAY","Uint32Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{29,"JS_CLASS_BIG_INT64_ARRAY","BigInt64Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{30,"JS_CLASS_BIG_UINT64_ARRAY","BigUint64Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{31,"JS_CLASS_FLOAT16_ARRAY","Float16Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{32,"JS_CLASS_FLOAT32_ARRAY","Float32Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{33,"JS_CLASS_FLOAT64_ARRAY","Float64Array",COVERED,"arena-baseobj: typed-array family",NULL,{NULL},NULL,NULL},
{34,"JS_CLASS_DATAVIEW","DataView",COVERED,"arena-baseobj: DataView setters refused",NULL,{NULL},NULL,NULL},
{35,"JS_CLASS_BIG_INT","BigInt",MUTATE,NULL,"Object(1n)",
    {"T.tag=1",NULL},"T.valueOf()","1"},
{36,"JS_CLASS_MAP","Map",COVERED,"arena-basemap: mutation throws",NULL,{NULL},NULL,NULL},
{37,"JS_CLASS_SET","Set",COVERED,"arena-basemap: mutation throws",NULL,{NULL},NULL,NULL},
{38,"JS_CLASS_WEAKMAP","WeakMap",COVERED,"arena-basemap: mutation throws",NULL,{NULL},NULL,NULL},
{39,"JS_CLASS_WEAKSET","WeakSet",COVERED,"arena-basemap: mutation throws",NULL,{NULL},NULL,NULL},
{40,"JS_CLASS_ITERATOR","Iterator",MUTATE,NULL,"Object.create(Iterator.prototype)",
    {"T.tag=1",NULL},"typeof T","object"},
{41,"JS_CLASS_ITERATOR_CONCAT","Iterator Concat",MUTATE,NULL,
    "Iterator.concat([1,2][Symbol.iterator]())",
    {"T.next()","T.tag=1",NULL},"T.next().value","1"},
{42,"JS_CLASS_ITERATOR_HELPER","Iterator Helper",MUTATE,NULL,
    "[1,2,3].values().map(x=>x)",
    {"T.next()","T.tag=1",NULL},"T.next().value","1"},
{43,"JS_CLASS_ITERATOR_WRAP","Iterator Wrap",MUTATE,NULL,
    "Iterator.from({ i:0, next(){ this.i++; return {value:this.i,done:this.i>3}; } })",
    {"T.next()",NULL},"T.next().value","1"},
{44,"JS_CLASS_MAP_ITERATOR","Map Iterator",MUTATE,NULL,
    "new Map([[1,'a'],[2,'b']]).keys()",
    {"T.next()",NULL},"T.next().value","1"},
{45,"JS_CLASS_SET_ITERATOR","Set Iterator",MUTATE,NULL,
    "new Set([1,2]).values()",
    {"T.next()",NULL},"T.next().value","1"},
{46,"JS_CLASS_ARRAY_ITERATOR","Array Iterator",MUTATE,NULL,
    "[1,2,3].values()",
    {"T.next()",NULL},"T.next().value","1"},
{47,"JS_CLASS_STRING_ITERATOR","String Iterator",MUTATE,NULL,
    "'ab'[Symbol.iterator]()",
    {"T.next()",NULL},"T.next().value","a"},
{48,"JS_CLASS_REGEXP_STRING_ITERATOR","RegExp String Iterator",MUTATE,NULL,
    "'aa'.matchAll(/a/g)",
    {"T.next()",NULL},"T.next().value[0]","a"},
{49,"JS_CLASS_GENERATOR","Generator",REFUSED,"a suspended frame cannot be copied non-arbitrarily; refused at freeze",
    "(function*(){ yield 1; yield 2; })()",
    {"T.next()",NULL},"T.next().value","1"},
{50,"JS_CLASS_DISPOSABLE_STACK","DisposableStack",MUTATE,NULL,
    "new DisposableStack()",
    {"T.use({[Symbol.dispose](){}})","T.defer(()=>{})",NULL},"T.disposed","false"},
{51,"JS_CLASS_PROXY","Object",MUTATE,NULL,"new Proxy({a:1},{})",
    {"T.a=9","T.b=1",NULL},"T.a","1"},
{52,"JS_CLASS_PROMISE","Promise",REFUSED,"a PENDING promise leaves a dangling reaction list; refused at freeze (settled is allowed)","new Promise(()=>{})",
    {"T.then(()=>{})","T.tag=1",NULL},"typeof T.then","function"},
/* Settle the promise before returning the function: an unresolved one
   left in the snapshot is itself a refusal hazard, which this test's own
   snapshot tripped when the refusal landed. */
{53,"JS_CLASS_PROMISE_RESOLVE_FUNCTION","PromiseResolveFunction",MUTATE,NULL,
    "(()=>{ let r; new Promise(res=>{ r=res; }); r(1); return r; })()",
    {"T.tag=1",NULL},"typeof T","function"},
/* Resolve (not reject — an unhandled rejection is noise) so the promise
   settles while the reject function is still captured. */
{54,"JS_CLASS_PROMISE_REJECT_FUNCTION","PromiseRejectFunction",MUTATE,NULL,
    "(()=>{ let j,r; new Promise((res,rej)=>{ r=res; j=rej; }); r(1); return j; })()",
    {"T.tag=1",NULL},"typeof T","function"},
{55,"JS_CLASS_ASYNC_FUNCTION","AsyncFunction",MUTATE,NULL,
    "(async function(){ return 1; })",
    {"T.tag=1",NULL},"typeof T","function"},
{56,"JS_CLASS_ASYNC_FUNCTION_RESOLVE","AsyncFunctionResolve",UNREACHABLE,
    "internal async-function continuation; never escapes to JS",NULL,{NULL},NULL,NULL},
{57,"JS_CLASS_ASYNC_FUNCTION_REJECT","AsyncFunctionReject",UNREACHABLE,
    "internal async-function continuation; never escapes to JS",NULL,{NULL},NULL,NULL},
{58,"JS_CLASS_ASYNC_FROM_SYNC_ITERATOR","",UNREACHABLE,
    "internal for-await adapter; never escapes to JS",NULL,{NULL},NULL,NULL},
{59,"JS_CLASS_ASYNC_GENERATOR_FUNCTION","AsyncGeneratorFunction",MUTATE,NULL,
    "(async function*(){ yield 1; })",
    {"T.tag=1",NULL},"typeof T","function"},
{60,"JS_CLASS_ASYNC_GENERATOR","AsyncGenerator",REFUSED,"a suspended frame cannot be copied non-arbitrarily; refused at freeze",
    "(async function*(){ yield 1; yield 2; })()",
    {"T.next()",NULL},"typeof T.next","function"},
{61,"JS_CLASS_ASYNC_DISPOSABLE_STACK","AsyncDisposableStack",MUTATE,NULL,
    "new AsyncDisposableStack()",
    {"T.use({[Symbol.asyncDispose](){}})","T.defer(()=>{})",NULL},"T.disposed","false"},
/* The target needs its own strong base reference: a WeakRef to an
   otherwise-unreferenced object legitimately derefs to undefined, which
   would read as a leak. */
{62,"JS_CLASS_WEAK_REF","WeakRef",MUTATE,NULL,
    "(globalThis.WR_TARGET = {a:1}, new WeakRef(globalThis.WR_TARGET))",
    {"T.deref()","T.tag=1",NULL},"typeof T.deref()","object"},
{63,"JS_CLASS_FINALIZATION_REGISTRY","FinalizationRegistry",REFUSED,"weak-ref bookkeeping lives in base and callbacks cannot fire; refused at freeze",
    "new FinalizationRegistry(()=>{})",
    {"T.register({},1)","T.tag=1",NULL},"typeof T.register","function"},
{64,"JS_CLASS_DOM_EXCEPTION","DOMException",MUTATE,NULL,
    "new DOMException('m','NotFoundError')",
    {"T.tag=1",NULL},"T.name","NotFoundError"},
{65,"JS_CLASS_CALL_SITE","CallSite",UNREACHABLE,
    "only produced for Error.prepareStackTrace; not retainable in a snapshot",NULL,{NULL},NULL,NULL},
{66,"JS_CLASS_RAWJSON","Object",NO_STATE,
    "JSON.rawJSON returns a frozen object; no mutable state",NULL,{NULL},NULL,NULL},
};

#define TABLE_LEN ((int)(sizeof TABLE / sizeof TABLE[0]))

static int nfail;
static int nred;   /* MUTATE classes that failed */
static int ngap;   /* KNOWN_GAP classes, still broken as expected */
static int nref;   /* REFUSED classes, refused as expected */

/* A REFUSED class gets its own runtime: build one instance, assert the
   hazard scan finds it, and never freeze. Freezing would abort — which
   is the refusal working, and useless as an assertion. */
static bool check_refused(const ClsEntry *e)
{
    JSRuntime *rt = JS_NewRuntimeArena(8 * 1024 * 1024, 8 * 1024 * 1024);
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    if (!ctx) { fprintf(stderr, "FAIL: %s: runtime setup\n", e->enum_name); return false; }

    /* Control: the scan must report nothing before we add the hazard. */
    bool ok = true;
    if (JS_ScanSnapshotHazards(rt, NULL) != 0) {
        fprintf(stderr, "FAIL: %-34s scan reported a hazard in an empty "
                        "snapshot\n", e->enum_name);
        ok = false;
    }

    char buf[1024];
    snprintf(buf, sizeof buf, "globalThis.HAZ = (%s), 1", e->ctor);
    JSValue v = JS_Eval(ctx, buf, strlen(buf), e->enum_name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        fprintf(stderr, "FAIL: %-34s constructor threw\n", e->enum_name);
        JS_FreeValue(ctx, JS_GetException(ctx));
        ok = false;
    }
    JS_FreeValue(ctx, v);

    FILE *devnull = fopen("/dev/null", "w");
    int n = JS_ScanSnapshotHazards(rt, devnull ? devnull : stderr);
    if (devnull) fclose(devnull);
    if (n < 1) {
        fprintf(stderr,
            "FAIL: %-34s is marked REFUSED but JS_ScanSnapshotHazards found\n"
            "      nothing. Either the refusal regressed, or this class is now\n"
            "      isolatable and the entry should become MUTATE.\n",
            e->enum_name);
        ok = false;
    }
    js_dual_arena_free(JS_GetDualArena(rt));
    return ok;
}

static JSValue eval_q(JSContext *ctx, const char *src, const char *label,
                      bool *threw)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), label, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        if (threw) *threw = true;
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    if (threw) *threw = false;
    return v;
}

/* Returns true if this class is clean. */
static bool check_class(JSRuntime *rt, JSContext *ctx, const ClsEntry *e,
                        bool expect_clean)
{
    char buf[1024];
    bool clean = true, threw;

    /* Build the instance in the snapshot... except we are already past
       freeze, so instead each request re-derives it from a base-resident
       factory captured at snapshot time. See main(): globalThis.MK_<id>
       holds the instance itself, built pre-freeze. */
    snprintf(buf, sizeof buf, "globalThis.T = MK_%u, 1", e->id);
    JS_ResetRequestArena(rt);
    JS_FreeValue(ctx, eval_q(ctx, buf, e->enum_name, &threw));
    if (threw) {
        fprintf(stderr, "FAIL: %-34s could not bind its instance\n", e->enum_name);
        nfail++;
        return false;
    }

    for (int i = 0; i < MAX_MUT && e->mut[i]; i++) {
        JS_ResetRequestArena(rt);
        snprintf(buf, sizeof buf, "globalThis.T = MK_%u; %s", e->id, e->mut[i]);
        js_arena_thermometer_reset();
        JS_FreeValue(ctx, eval_q(ctx, buf, e->enum_name, &threw));
        size_t pages = js_arena_thermometer_pages();
        if (pages != 0) {
            fprintf(expect_clean ? stderr : stdout,
                "  %s  %-24s `%s` dirtied %zu page(s), %zu byte(s)%s\n",
                expect_clean ? "BASE WRITE" : "gap:      ",
                e->enum_name, e->mut[i], pages,
                js_arena_thermometer_changed_bytes(), threw ? " (threw)" : "");
            clean = false;
        }
    }

    /* Isolation: mutate, reset, and read the probe. */
    if (e->probe) {
        JS_ResetRequestArena(rt);
        for (int i = 0; i < MAX_MUT && e->mut[i]; i++) {
            snprintf(buf, sizeof buf, "globalThis.T = MK_%u; %s", e->id, e->mut[i]);
            JS_FreeValue(ctx, eval_q(ctx, buf, e->enum_name, &threw));
        }
        JS_ResetRequestArena(rt);
        snprintf(buf, sizeof buf, "globalThis.T = MK_%u; String(%s)",
                 e->id, e->probe);
        JSValue v = eval_q(ctx, buf, e->enum_name, &threw);
        const char *got = threw ? NULL : JS_ToCString(ctx, v);
        if (!got || strcmp(got, e->expect) != 0) {
            fprintf(expect_clean ? stderr : stdout,
                "  %s  %-24s `%s` -> '%s' after a previous request "
                "mutated it; expected '%s'\n",
                expect_clean ? "LEAK      " : "gap:      ",
                e->enum_name, e->probe, got ? got : "(threw)", e->expect);
            clean = false;
        }
        if (got) JS_FreeCString(ctx, got);
        JS_FreeValue(ctx, v);
    }
    return clean;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntimeArena(32 * 1024 * 1024, 32 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "JS_NewRuntimeArena failed\n"); return 2; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 2; }

    /* ---- table vs engine: every registered class must be classified ---- */
    printf("class table (%d entries):\n", TABLE_LEN);
    for (JSClassID id = 1; id < 256; id++) {
        if (!JS_IsRegisteredClass(rt, id))
            continue;
        const ClsEntry *e = NULL;
        for (int i = 0; i < TABLE_LEN; i++)
            if (TABLE[i].id == id) { e = &TABLE[i]; break; }
        if (!e) {
            JSAtom a = JS_GetClassName(rt, id);
            const char *nm = JS_AtomToCString(ctx, a);
            fprintf(stderr,
                "FAIL: class id %u (\"%s\") is registered but not classified.\n"
                "      Add an entry to TABLE in arena-baseclass.c saying whether a\n"
                "      base-resident instance can be mutated, and assert it.\n",
                (unsigned)id, nm ? nm : "?");
            if (nm) JS_FreeCString(ctx, nm);
            JS_FreeAtom(ctx, a);
            nfail++;
            continue;
        }
        JSAtom a = JS_GetClassName(rt, id);
        const char *nm = JS_AtomToCString(ctx, a);
        if (!nm || strcmp(nm, e->class_name) != 0) {
            fprintf(stderr,
                "FAIL: class id %u reports name \"%s\", table says \"%s\" (%s).\n"
                "      The JS_CLASS_* enum moved under the table; re-key it.\n",
                (unsigned)id, nm ? nm : "?", e->class_name, e->enum_name);
            nfail++;
        }
        if (nm) JS_FreeCString(ctx, nm);
        JS_FreeAtom(ctx, a);
    }
    for (int i = 0; i < TABLE_LEN; i++) {
        if (!JS_IsRegisteredClass(rt, TABLE[i].id)) {
            fprintf(stderr, "FAIL: table entry %s (id %u) is not a registered "
                            "class; stale entry.\n",
                    TABLE[i].enum_name, TABLE[i].id);
            nfail++;
        }
    }

    /* ---- build one base-resident instance per MUTATE class ---- */
    int n_mut = 0, n_skip = 0;
    for (int i = 0; i < TABLE_LEN; i++) {
        const ClsEntry *e = &TABLE[i];
        if (e->kind != MUTATE && e->kind != KNOWN_GAP) { n_skip++; continue; }
        /* REFUSED entries are handled in check_refused(), in their own runtime. */
        char buf[1024];
        snprintf(buf, sizeof buf, "globalThis.MK_%u = (%s), 1", e->id, e->ctor);
        bool threw;
        JS_FreeValue(ctx, eval_q(ctx, buf, e->enum_name, &threw));
        if (threw) {
            fprintf(stderr,
                "FAIL: %-34s constructor did not evaluate: %s\n"
                "      Fix the expression, or reclassify the entry.\n",
                e->enum_name, e->ctor);
            nfail++;
            continue;
        }
        n_mut++;
    }
    if (js_dual_arena_oom_hit(JS_GetDualArena(rt)))
        fprintf(stderr, "WARNING: request-arena OOM latched pre-freeze\n");

    JS_FreezeRuntime(rt);
    js_dual_arena_set_request_mode(JS_GetDualArena(rt), JS_ARENA_REQ_MODE_BUMP);
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 2;
    }

    printf("  %d classes exercised, %d classified without a live instance\n\n",
           n_mut, n_skip);

    for (int i = 0; i < TABLE_LEN; i++) {
        const ClsEntry *e = &TABLE[i];
        if (e->kind == REFUSED) {
            if (check_refused(e))
                printf("  REF %-34s %s\n", e->enum_name, e->note);
            else
                nfail++;
            nref++;
            continue;
        }
        if (e->kind == KNOWN_GAP) {
            if (check_class(rt, ctx, e, false)) {
                fprintf(stderr,
                    "FAIL: %-34s is marked KNOWN_GAP but is now clean.\n"
                    "      Promote it to MUTATE and delete the gap note — a fix\n"
                    "      nobody records is a fix that gets undone.\n",
                    e->enum_name);
                nfail++;
            } else {
                printf("  GAP %-34s %s\n", e->enum_name, e->note);
                ngap++;
            }
            continue;
        }
        if (e->kind != MUTATE) {
            printf("  --  %-34s %s\n", e->enum_name, e->note);
            continue;
        }
        if (check_class(rt, ctx, e, true)) {
            printf("  ok  %-34s clean\n", e->enum_name);
        } else {
            nred++;
        }
    }

    js_arena_thermometer_disable();
    fflush(stdout);

    if (nfail || nred) {
        fprintf(stderr,
            "\n%d class(es) mutate base or leak across a reset; "
            "%d structural failure(s).\n", nred, nfail);
        return 1;
    }
    printf("\nPASS: every registered class is classified; %d clean, %d "
           "refused at freeze, %d known gap(s).\n", n_mut - ngap, nref, ngap);
    if (ngap)
        printf("      Known gaps are asserted to still be broken — fixing one "
               "turns this test red\n      until the entry is promoted.\n");
    return 0;
}
