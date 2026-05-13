// Stack-state-inspection smoke test.
//
// Stops on a chosen event index (the natural scrubber control mode —
// "stop after the Nth event") rather than on line numbers, since
// QJS's pc2line table is sparse and exact line emission is
// script-dependent. Verifies that snapshot delivered via host_state
// has the expected frames + locals.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const arena_init           = M.cwrap("arena_init",           "number", ["number","number"]);
const arena_run_module     = M.cwrap("arena_run_module",     "number", ["string","string"]);
const arena_set_trace_mode = M.cwrap("arena_set_trace_mode", null,     ["number"]);
const arena_destroy        = M.cwrap("arena_destroy",        null,     []);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

const TRACE_DRILL = 2;
const K_NAME = 0;
const dec = new TextDecoder();

let stopAtEvent = -1, eventCount = 0;
let snapshot = null;
let observedEvents = [];   // captured for cases that just want to introspect

M.host_trace = (kind, ptr, len) => {
    if (kind === K_NAME) {
        // Don't count NAME events against the stop counter — they're
        // out-of-band bookkeeping that fires once per new atom.
        return 0;
    }
    eventCount++;
    observedEvents.push({ kind, eventCount });
    return eventCount === stopAtEvent ? 2 : 0;
};

M.host_state = (ptr, len) => {
    snapshot = JSON.parse(dec.decode(M.HEAPU8.subarray(ptr, ptr + len)));
};

function reset(stopAt) {
    stopAtEvent = stopAt;
    eventCount = 0;
    snapshot = null;
    observedEvents = [];
}

let passed = 0, total = 0;
function check(label, ok, ...rest) {
    total++;
    if (ok) passed++;
    else console.log("!! " + label, ...rest);
}

arena_set_trace_mode(TRACE_DRILL);

// First pass to discover where the meaningful stop points are for
// each script. Then a second pass that stops at the chosen event and
// inspects.

function discover(label, name, src) {
    reset(-1);
    const rc = arena_run_module(name, src);
    return { rc, events: observedEvents };
}

function stopAtAndInspect(name, src, stopEv) {
    reset(stopEv);
    const rc = arena_run_module(name, src);
    return { rc, events: observedEvents, snap: snapshot };
}

// ── Case 1: inspect inside a function with args + locals ─────────────
{
    const src =
`function f(x, y) {
  let z = x + y;
  let w = z * 2;
  return w;
}
f(10, 20);`;

    // Discover what events fire. Find the last LINE event in file f.js
    // — that's the last user-observable position before f returns.
    const d = discover("case1", "f.js", src);
    // Identify "inside f" stop — kind 3 (LINE) inside f's frame.
    // Easiest: stop on a specific known index from the trace order.
    // Events order (from manual observation):
    //   ENTER <eval>(1), LINE(1), EXIT, ENTER <eval>(2), LINE, LINE,
    //   ENTER f, LINE 1, LINE 2, LINE 3, EXIT f, LINE, EXIT
    // The last LINE inside f is the 10th event. Let's verify by
    // counting LINE-after-ENTER-f and stopping at the deepest one.
    let depth = 0, deepestEvent = -1;
    for (const e of d.events) {
        if (e.kind === 1) depth++;
        else if (e.kind === 2) depth--;
        else if (e.kind === 3 && depth >= 2) deepestEvent = e.eventCount;
    }
    check("case1: found a deep LINE event", deepestEvent > 0, deepestEvent);

    const r = stopAtAndInspect("f.js", src, deepestEvent);
    check("case1: stop rc=0", r.rc === 0);
    check("case1: snapshot delivered", r.snap !== null);
    if (r.snap) {
        const f = r.snap.find(fr => fr.func === "f");
        check("case1: f frame present", !!f);
        if (f) {
            check("case1: x=10", f.vars.x === 10, f.vars);
            check("case1: y=20", f.vars.y === 20, f.vars);
            // z and w may or may not be set depending on which exact
            // LINE event we stopped at; assert they exist as keys
            // (uninitialised gets stringified to "[uninitialised]").
            check("case1: z present", "z" in f.vars, Object.keys(f.vars));
            check("case1: w present", "w" in f.vars, Object.keys(f.vars));
        }
        const evalFrame = r.snap.find(fr => fr.func === "<eval>");
        check("case1: <eval> frame underneath", !!evalFrame);
    }
}

// ── Case 2: many value shapes serialise correctly ───────────────────
//
// Wrap in a function so vars live in arg_buf/var_buf where the walker
// can see them. Module-level `let` bindings live in the module's
// lexical environment (closure-var slots) which v1 doesn't enumerate;
// that's a known limitation worth documenting separately.
{
    const src =
`function shapes() {
  let n   = 42;
  let s   = "hi";
  let arr = [1,2,3];
  let obj = { x: 1, y: [true, null] };
  let fn  = function named() {};
  let u;
  return n + (u === undefined ? 0 : 0);
}
shapes();`;

    const d = discover("case2", "v.js", src);
    let depth = 0, deepest = -1;
    for (const e of d.events) {
        if (e.kind === 1) depth++;
        else if (e.kind === 2) depth--;
        else if (e.kind === 3 && depth >= 2) deepest = e.eventCount;
    }
    check("case2: found deep LINE", deepest > 0, deepest);

    const r = stopAtAndInspect("v.js", src, deepest);
    check("case2: stop rc=0", r.rc === 0);
    check("case2: snapshot delivered", r.snap !== null);
    if (r.snap) {
        const f = r.snap.find(fr => fr.func === "shapes");
        check("case2: shapes frame found", !!f);
        if (f) {
            check("case2: number n=42", f.vars.n === 42, f.vars);
            check("case2: string s='hi'", f.vars.s === "hi", f.vars);
            check("case2: array arr=[1,2,3]",
                  Array.isArray(f.vars.arr) && f.vars.arr.length === 3, f.vars);
            check("case2: nested object",
                  f.vars.obj?.x === 1 && f.vars.obj?.y?.[0] === true, f.vars);
            check("case2: function placeholder",
                  typeof f.vars.fn === "string" && f.vars.fn.startsWith("[function"),
                  f.vars);
            check("case2: undefined placeholder",
                  f.vars.u === "[undefined]", f.vars);
        }
    }
}

// ── Case 3: nested calls produce multiple frames ─────────────────────
{
    const src =
`function leaf(z)   { let q = z + 1;
                      return q; }
function mid(y)    { return leaf(y * 2); }
function top()     { return mid(5); }
top();`;

    // Stop on the deepest LINE event — should be inside `leaf`.
    const d = discover("case3", "t3.js", src);
    let depth = 0, maxDepth = 0, stopEv = -1;
    for (const e of d.events) {
        if (e.kind === 1) {
            depth++;
            if (depth > maxDepth) maxDepth = depth;
        } else if (e.kind === 2) depth--;
        else if (e.kind === 3 && depth === maxDepth) stopEv = e.eventCount;
    }
    check("case3: found deepest LINE", stopEv > 0, { stopEv, maxDepth });

    const r = stopAtAndInspect("t3.js", src, stopEv);
    check("case3: stop rc=0", r.rc === 0);
    check("case3: snapshot delivered", r.snap !== null);
    if (r.snap) {
        const funcs = r.snap.map(f => f.func);
        check("case3: leaf is innermost", funcs[0] === "leaf", funcs);
        check("case3: chain includes mid", funcs.includes("mid"), funcs);
        check("case3: chain includes top", funcs.includes("top"), funcs);
        check("case3: chain reaches <eval>", funcs.includes("<eval>"), funcs);
        const leaf = r.snap[0];
        check("case3: leaf.z=10", leaf.vars.z === 10, leaf.vars);
    }
}

// ── Case 4: closure captures — inner function sees outer's locals ────
//
// `inner` captures `count` and `prefix` from `outer`. When we stop
// inside `inner`, BOTH `inner`'s own locals AND the closure-captured
// names from `outer`'s scope should be visible — the latter via
// b->closure_var, the former via b->vardefs.
{
    const src =
`function outer() {
  let count = 5;
  let prefix = "n=";
  function inner(x) {
    let label = prefix + x;
    let total = count + x;
    return label + " total=" + total;
  }
  return inner(2);
}
outer();`;

    const d = discover("case4", "c.js", src);
    let depth = 0, maxDepth = 0, stopEv = -1;
    for (const e of d.events) {
        if (e.kind === 1) {
            depth++;
            if (depth > maxDepth) maxDepth = depth;
        } else if (e.kind === 2) depth--;
        else if (e.kind === 3 && depth === maxDepth) stopEv = e.eventCount;
    }
    check("case4: found deepest LINE", stopEv > 0, { stopEv, maxDepth });

    const r = stopAtAndInspect("c.js", src, stopEv);
    check("case4: stop rc=0", r.rc === 0);
    check("case4: snapshot delivered", r.snap !== null);
    if (r.snap) {
        const inner = r.snap.find(f => f.func === "inner");
        check("case4: inner frame present", !!inner);
        if (inner) {
            // Own arg + locals
            check("case4: inner.x=2", inner.vars.x === 2, inner.vars);
            // Closure-captured from outer:
            check("case4: closure count=5", inner.vars.count === 5, inner.vars);
            check("case4: closure prefix='n='",
                  inner.vars.prefix === "n=", inner.vars);
        }
    }
}

// ── Case 5: module top-level `let` visible to inner function ─────────
//
// Module body's top-level `let`s end up as captured locals on the
// module-body frame AND show up as closure_var entries on any inner
// function that references them. Verifies the closure-var walker
// surfaces module-scope state when we stop inside a handler.
{
    const src =
`let appName = "rove";
let counter = 1;
function bump(n) {
  counter = counter + n;
  return appName + ":" + counter;
}
bump(7);`;

    const d = discover("case5", "m.js", src);
    let depth = 0, maxDepth = 0, stopEv = -1;
    for (const e of d.events) {
        if (e.kind === 1) {
            depth++;
            if (depth > maxDepth) maxDepth = depth;
        } else if (e.kind === 2) depth--;
        else if (e.kind === 3 && depth === maxDepth) stopEv = e.eventCount;
    }
    check("case5: found deep LINE", stopEv > 0, stopEv);

    const r = stopAtAndInspect("m.js", src, stopEv);
    check("case5: stop rc=0", r.rc === 0);
    check("case5: snapshot delivered", r.snap !== null);
    if (r.snap) {
        const bump = r.snap.find(f => f.func === "bump");
        check("case5: bump frame found", !!bump);
        if (bump) {
            check("case5: bump.n=7", bump.vars.n === 7, bump.vars);
            check("case5: closure appName='rove'",
                  bump.vars.appName === "rove", bump.vars);
            // counter may or may not be visible as the post-update
            // value depending on where exactly we stopped; assert it
            // exists as a key with some integer value.
            check("case5: closure counter present",
                  typeof bump.vars.counter === "number", bump.vars);
        }
        // Module body frame should expose the top-level lets too.
        const evalFrame = r.snap.find(f => f.func === "<eval>");
        if (evalFrame) {
            check("case5: <eval> appName visible",
                  evalFrame.vars.appName === "rove", evalFrame.vars);
            check("case5: <eval> counter visible",
                  typeof evalFrame.vars.counter === "number", evalFrame.vars);
        }
    }
}

arena_destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
