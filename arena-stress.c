/*
 * Kitchen-sink stress test: runs a wide variety of JS surface area
 * under the thermometer, asserting zero base writes per request.
 *
 * Each script is a self-contained workload that exercises a different
 * area of the runtime (Array methods, Object methods, prototype-chain
 * walks, getters/setters, exceptions, JSON, RegExp, Map/Set, etc.).
 * The asserter inside each script fails the run if the result is
 * wrong; the C harness fails it if the thermometer detects any base
 * write during the eval.
 *
 * Build:
 *   cc -I. -O2 arena-stress.c build/libqjs.a -lm -lpthread -o build/arena-stress
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "qjs-arena.h"

typedef struct { const char *name; const char *src; } Test;

static const Test TESTS[] = {
    /* Array prototype chain */
    { "array.push.length",        "let a=[1,2,3]; a.push(4,5); if(a.length!==5)throw'X'; 'ok'" },
    { "array.map.filter.reduce",  "let r=[1,2,3,4,5].map(x=>x*x).filter(x=>x>5).reduce((a,b)=>a+b,0); if(r!==50)throw'X:'+r; 'ok'" },
    { "array.from.iter",          "let r=Array.from('abc',c=>c.toUpperCase()).join(''); if(r!=='ABC')throw'X'; 'ok'" },
    { "array.sort",               "let a=[3,1,4,1,5,9,2,6]; a.sort((x,y)=>x-y); if(a.join(',')!=='1,1,2,3,4,5,6,9')throw'X'; 'ok'" },
    { "array.flat.flatMap",       "let r=[[1,2],[3,4]].flat().flatMap(x=>[x,x*2]); if(r.length!==8)throw'X'; 'ok'" },
    { "array.find.findIndex",     "let r=[1,2,3,4].findIndex(x=>x>2); if(r!==2)throw'X'; 'ok'" },

    /* Object prototype */
    { "object.keys.values",       "let o={a:1,b:2,c:3}; if(Object.keys(o).length!==3||Object.values(o).reduce((a,b)=>a+b,0)!==6)throw'X'; 'ok'" },
    { "object.assign",            "let o=Object.assign({},{a:1},{b:2}); if(o.a+o.b!==3)throw'X'; 'ok'" },
    { "object.entries.fromEntries","let r=Object.fromEntries(Object.entries({x:1,y:2})); if(r.x+r.y!==3)throw'X'; 'ok'" },
    { "object.getOwnPropertyNames","let r=Object.getOwnPropertyNames({a:1,b:2}); if(r.length!==2)throw'X'; 'ok'" },

    /* String */
    { "string.split.join",        "if('a,b,c'.split(',').join('-')!=='a-b-c')throw'X'; 'ok'" },
    { "string.replace.regex",     "if('hello'.replace(/l/g,'L')!=='heLLo')throw'X'; 'ok'" },
    { "string.match.regex",       "if('a1b2c3'.match(/\\d/g).length!==3)throw'X'; 'ok'" },
    { "string.template",          "let n=42; if(`x=${n}`!=='x=42')throw'X'; 'ok'" },

    /* JSON */
    { "json.parse.stringify",     "let o={a:[1,2,{b:'c'}]}; if(JSON.parse(JSON.stringify(o)).a[2].b!=='c')throw'X'; 'ok'" },

    /* Math */
    { "math.basics",              "if(Math.max(1,2,3)!==3||Math.floor(2.7)!==2||Math.abs(-5)!==5)throw'X'; 'ok'" },

    /* Map/Set */
    { "map.set.has.get",          "let m=new Map([['a',1],['b',2]]); m.set('c',3); if(m.get('c')!==3||m.size!==3)throw'X'; 'ok'" },
    { "set.has",                  "let s=new Set([1,2,3]); s.add(4); if(!s.has(2)||s.size!==4)throw'X'; 'ok'" },
    { "weakmap",                  "let k={}; let m=new WeakMap(); m.set(k,42); if(m.get(k)!==42)throw'X'; 'ok'" },

    /* Closures + scope */
    { "closure.capture",          "let f=(x=>(y=>x+y))(10); if(f(5)!==15)throw'X'; 'ok'" },
    { "iife",                     "let r=(function(){let x=1,y=2;return x+y})(); if(r!==3)throw'X'; 'ok'" },

    /* Class */
    { "class.basic",              "class P{constructor(n){this.n=n};greet(){return'hi '+this.n}} if(new P('x').greet()!=='hi x')throw'X'; 'ok'" },
    { "class.inherit",            "class A{f(){return 1}} class B extends A{f(){return super.f()+1}} if(new B().f()!==2)throw'X'; 'ok'" },
    { "class.static",             "class K{static n=42;static get(){return K.n}} if(K.get()!==42)throw'X'; 'ok'" },

    /* try/catch */
    { "try.catch",                "let r=0; try{throw 'err'}catch(e){r=e.length} if(r!==3)throw'X'; 'ok'" },
    { "try.finally",              "let r=0; try{r=1}finally{r=2} if(r!==2)throw'X'; 'ok'" },

    /* Generator */
    { "generator",                "function*g(){yield 1;yield 2;yield 3} let s=0; for(let v of g())s+=v; if(s!==6)throw'X'; 'ok'" },

    /* Destructuring + spread */
    { "destructure.array",        "let [a,b,...c]=[1,2,3,4,5]; if(a+b!==3||c.length!==3)throw'X'; 'ok'" },
    { "destructure.object",       "let {x,y:z=10}=({x:1}); if(x!==1||z!==10)throw'X'; 'ok'" },
    { "spread.call",              "if(Math.max(...[1,5,3])!==5)throw'X'; 'ok'" },

    /* RegExp */
    { "regex.exec",               "let m=/(\\d+)/.exec('abc123'); if(m[1]!=='123')throw'X'; 'ok'" },
    { "regex.named",              "let m=/(?<n>\\d+)/.exec('a42'); if(m.groups.n!=='42')throw'X'; 'ok'" },

    /* Date — touches base time machinery */
    { "date.basic",               "let d=new Date(0); if(d.getTime()!==0)throw'X'; 'ok'" },

    /* Number */
    { "number.parse",             "if(parseInt('42')!==42||parseFloat('3.14')!==3.14)throw'X'; 'ok'" },

    /* Symbol + iteration */
    { "symbol.iterator",          "let it={[Symbol.iterator](){let i=0;return{next(){return{value:i,done:i++>=3}}}}}; let s=0; for(let v of it)s+=v; if(s!==3)throw'X'; 'ok'" },

    /* Proxy */
    { "proxy.basic",              "let p=new Proxy({},{get:(t,k)=>k.toUpperCase()}); if(p.hello!=='HELLO')throw'X'; 'ok'" },

    /* Object.defineProperty getter/setter */
    { "defineProperty.getset",    "let o={};let v=0;Object.defineProperty(o,'x',{get(){return v},set(n){v=n}}); o.x=5; if(o.x!==5)throw'X'; 'ok'" },

    /* Promise (sync resolve only — no microtask flush in this driver) */
    { "promise.then",             "Promise.resolve(42).then(v=>{if(v!==42)throw'X'}); 'ok'" },
};

#define NTESTS (sizeof(TESTS)/sizeof(TESTS[0]))

int main(int argc, char **argv)
{
    int strict = argc > 1 && strcmp(argv[1], "--lax") == 0 ? 0 : 1;

    JSRuntime *rt = JS_NewRuntimeArena(16 * 1024 * 1024, 16 * 1024 * 1024);
    if (!rt) { fprintf(stderr, "arena create failed\n"); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "ctx create failed\n"); return 1; }
    JS_FreezeRuntime(rt);
    if (js_arena_thermometer_enable() < 0) {
        fprintf(stderr, "thermometer enable failed\n"); return 1;
    }

    int total_pass = 0, total_fail_logic = 0, total_fail_base = 0;
    size_t worst_bytes = 0;
    const char *worst_test = NULL;

    for (size_t i = 0; i < NTESTS; i++) {
        js_arena_thermometer_reset();
        /* For diagnosis: if we want to trace one specific test, set
           the env var ARENA_TRACE to its name. */
        const char *trace_name = getenv("ARENA_TRACE");
        if (trace_name && strcmp(trace_name, TESTS[i].name) == 0)
            js_arena_thermometer_trace_range(0, 16 * 1024 * 1024);
        JSValue v = JS_Eval(ctx, TESTS[i].src, strlen(TESTS[i].src),
                            TESTS[i].name, JS_EVAL_TYPE_GLOBAL);
        if (trace_name && strcmp(trace_name, TESTS[i].name) == 0)
            js_arena_thermometer_trace_range(0, 0);
        size_t pages  = js_arena_thermometer_pages();
        size_t bytes  = js_arena_thermometer_changed_bytes();
        bool exc = JS_IsException(v);
        const char *err = NULL;
        if (exc) {
            JSValue e = JS_GetException(ctx);
            err = JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
        }
        bool logic_ok = !exc;
        bool base_ok  = (pages == 0 && bytes == 0);

        if (!logic_ok) {
            total_fail_logic++;
            printf("LOGIC %-30s %s\n", TESTS[i].name, err ? err : "(no msg)");
        } else if (!base_ok) {
            total_fail_base++;
            printf("BASE  %-30s pages=%zu bytes=%zu\n",
                   TESTS[i].name, pages, bytes);
            if (bytes > worst_bytes) {
                worst_bytes = bytes;
                worst_test = TESTS[i].name;
            }
        } else {
            total_pass++;
        }
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, v);
        JS_ResetRequestArena(rt);
    }

    printf("\n=== summary ===\n");
    printf("  %d passed (clean eval, zero base writes)\n", total_pass);
    printf("  %d failed: logic / exception\n", total_fail_logic);
    printf("  %d failed: base mutation (pages > 0)\n", total_fail_base);
    if (worst_test)
        printf("  worst: %s wrote %zu bytes\n", worst_test, worst_bytes);

    js_arena_thermometer_disable();
    js_dual_arena_free(JS_GetDualArena(rt));
    return strict && (total_fail_logic + total_fail_base) > 0;
}
