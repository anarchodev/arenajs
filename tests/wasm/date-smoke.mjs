// `new Date()` override smoke test.
//
// Verifies that the no-arg Date constructor (both `new Date()` and the
// `Date()` non-new call) consume the same tape as `Date.now()`, while
// passing through any explicit-args / explicit-ms construction. Also
// asserts that `instanceof Date`, `Date.UTC`, and `Date.prototype`
// methods survive the wrapper installation.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const init    = M.cwrap("arena_init", "number", ["number","number"]);
const runmod  = M.cwrap("arena_run_module", "number", ["string","string"]);
const destroy = M.cwrap("arena_destroy", null, []);
if (init(8192, 8192) !== 0) throw new Error("arena_init failed");

M.tapes = {}; M.module_sources = {};
let passed = 0, total = 0;
function check(label, ok) { total++; if (ok) passed++; else console.log(`!! ${label}`); }
function fresh(tapes) {
    for (const k of Object.keys(tapes)) tapes[k]._cursor = 0;
    M.tapes = tapes;
}

// ── `new Date()` (no args) pulls from tape, mixed with Date.now() ────
//
// Tape order matches call order: handler does Date.now(), new Date(),
// new Date() — three consecutive entries. The wrapper for new Date()
// internally calls Date.now() (our tape-bound one), so each `new Date()`
// consumes exactly one entry.
fresh({ date: [{ ms: 1000 }, { ms: 2000 }, { ms: 3000 }] });
check("no-arg new Date + Date.now share tape", runmod("e1.js", `
    const a = Date.now();
    const b = new Date();
    const c = new Date();
    if (a !== 1000) throw new Error("Date.now: " + a);
    if (b.getTime() !== 2000) throw new Error("new Date #1: " + b.getTime());
    if (c.getTime() !== 3000) throw new Error("new Date #2: " + c.getTime());
`) === 0);

// ── explicit-args new Date() passes through, no tape consumed ────────
fresh({ date: [{ ms: 9999 }] });
check("explicit ms passes through", runmod("e2.js", `
    const d = new Date(500);
    if (d.getTime() !== 500) throw new Error("explicit ms: " + d.getTime());
    // Tape still has its first entry — consume to prove cursor didn't move.
    const x = Date.now();
    if (x !== 9999) throw new Error("tape advanced unexpectedly: " + x);
`) === 0);

// ── multi-arg constructor passes through ─────────────────────────────
fresh({ date: [] });
check("multi-arg constructor", runmod("e3.js", `
    // (2024, 5, 15, 12, 30, 0) → June 15 2024 12:30:00 local time
    const d = new Date(2024, 5, 15, 12, 30, 0);
    if (d.getFullYear() !== 2024) throw new Error("year: " + d.getFullYear());
    if (d.getMonth()   !== 5)    throw new Error("month: " + d.getMonth());
    if (d.getDate()    !== 15)   throw new Error("date: " + d.getDate());
`) === 0);

// ── instanceof Date still works ──────────────────────────────────────
fresh({ date: [{ ms: 42 }] });
check("instanceof Date", runmod("e4.js", `
    const d = new Date();
    if (!(d instanceof Date)) throw new Error("instanceof failed");
    if (d.getTime() !== 42) throw new Error("ms: " + d.getTime());
`) === 0);

// ── Date.UTC and other statics preserved ─────────────────────────────
fresh({ date: [] });
check("static methods (Date.UTC, Date.parse)", runmod("e5.js", `
    const ms = Date.UTC(2024, 0, 1);                   // Jan 1 2024 00:00 UTC
    if (ms !== 1704067200000) throw new Error("Date.UTC: " + ms);
    const p = Date.parse("2024-01-01T00:00:00.000Z");
    if (p !== 1704067200000) throw new Error("Date.parse: " + p);
`) === 0);

// ── prototype methods (toISOString etc.) work on tape-built Date ─────
fresh({ date: [{ ms: 1704067200000 }] });  // 2024-01-01T00:00:00.000Z
check("prototype methods on tape Date", runmod("e6.js", `
    const d = new Date();
    const iso = d.toISOString();
    if (iso !== "2024-01-01T00:00:00.000Z") throw new Error("toISOString: " + iso);
`) === 0);

// ── divergence: tape exhausted while wrapper still being called ──────
fresh({ date: [{ ms: 100 }] });
check("tape exhausted on second new Date()", runmod("e7.js", `
    let _ = new Date();
    let threw = false;
    try { _ = new Date(); } catch (e) { threw = true; }
    if (!threw) throw new Error("expected exhaustion throw");
`) === 0);

destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
