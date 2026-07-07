// Pinned-clock smoke test for Date.now() / no-arg `new Date()`.
//
// Contract (see arena_set_date_now in qjs-arena-reactor.c): the host
// pins Date.now() to a fixed UTC-ms value per request via
// arena_set_date_now(lo, hi); within that request every Date.now()
// and no-arg `new Date()` (and the non-new `Date()` call) reads the
// pinned value — there is no per-read tape and no exhaustion. The pin
// persists until the host sets a new one; explicit-args construction,
// Date statics, instanceof, and prototype methods are unaffected.
//
// (This file predates the seed-not-draws change and used to assert a
// `date` tape with one entry consumed per read. That model is gone —
// replay determinism for clocks comes from the single pinned value.)

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const init    = M.cwrap("arena_init", "number", ["number","number"]);
const runmod  = M.cwrap("arena_run_module", "number", ["string","string"]);
const setdate = M.cwrap("arena_set_date_now", null, ["number","number"]);
const destroy = M.cwrap("arena_destroy", null, []);
if (init(8192, 8192) !== 0) throw new Error("arena_init failed");

M.tapes = {}; M.module_sources = {};
let passed = 0, total = 0;
function check(label, ok) { total++; if (ok) passed++; else console.log(`!! ${label}`); }
function pin(ms) {
    setdate(ms >>> 0, Math.floor(ms / 4294967296));
}

// ── unpinned: Date.now() reads the real clock ────────────────────────
check("unpinned reads real clock", runmod("d0.js", `
    const t = Date.now();
    if (!(t > 1700000000000)) throw new Error("expected wall clock, got " + t);
`) === 0);

// ── pinned: Date.now() and no-arg new Date() agree, repeatedly ───────
pin(1000);
check("Date.now / new Date pinned and stable", runmod("d1.js", `
    const a = Date.now();
    const b = new Date();
    const c = new Date();
    if (a !== 1000) throw new Error("Date.now: " + a);
    if (b.getTime() !== 1000) throw new Error("new Date #1: " + b.getTime());
    if (c.getTime() !== 1000) throw new Error("new Date #2: " + c.getTime());
    if (Date.now() !== 1000) throw new Error("re-read drifted");
`) === 0);

// ── explicit-args construction ignores the pin ───────────────────────
pin(9999);
check("explicit ms passes through", runmod("d2.js", `
    const d = new Date(500);
    if (d.getTime() !== 500) throw new Error("explicit ms: " + d.getTime());
    if (Date.now() !== 9999) throw new Error("pin lost: " + Date.now());
`) === 0);

// ── multi-arg constructor passes through ─────────────────────────────
check("multi-arg constructor", runmod("d3.js", `
    // (2024, 5, 15, 12, 30, 0) → June 15 2024 12:30:00 local time
    const d = new Date(2024, 5, 15, 12, 30, 0);
    if (d.getFullYear() !== 2024) throw new Error("year: " + d.getFullYear());
    if (d.getMonth()   !== 5)    throw new Error("month: " + d.getMonth());
    if (d.getDate()    !== 15)   throw new Error("date: " + d.getDate());
`) === 0);

// ── instanceof Date on a pinned-clock Date ───────────────────────────
pin(42);
check("instanceof Date", runmod("d4.js", `
    const d = new Date();
    if (!(d instanceof Date)) throw new Error("instanceof failed");
    if (d.getTime() !== 42) throw new Error("ms: " + d.getTime());
`) === 0);

// ── Date.UTC and other statics preserved ─────────────────────────────
check("static methods (Date.UTC, Date.parse)", runmod("d5.js", `
    const ms = Date.UTC(2024, 0, 1);                   // Jan 1 2024 00:00 UTC
    if (ms !== 1704067200000) throw new Error("Date.UTC: " + ms);
    const p = Date.parse("2024-01-01T00:00:00.000Z");
    if (p !== 1704067200000) throw new Error("Date.parse: " + p);
`) === 0);

// ── prototype methods on a pinned-clock Date ─────────────────────────
pin(1704067200000);  // 2024-01-01T00:00:00.000Z
check("prototype methods on pinned Date", runmod("d6.js", `
    const d = new Date();
    const iso = d.toISOString();
    if (iso !== "2024-01-01T00:00:00.000Z") throw new Error("toISOString: " + iso);
`) === 0);

// ── re-pin between requests: each run sees its own clock ─────────────
pin(2000);
check("request A sees its pin", runmod("d7a.js", `
    if (Date.now() !== 2000) throw new Error("pin A: " + Date.now());
`) === 0);
pin(3000);
check("request B sees the new pin", runmod("d7b.js", `
    if (Date.now() !== 3000) throw new Error("pin B: " + Date.now());
`) === 0);

destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
