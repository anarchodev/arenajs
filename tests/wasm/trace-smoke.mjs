// Trace-emitter smoke test.
//
// Sets up Module.host_trace to collect binary events, runs a script in
// scan and drill modes, and verifies the event stream against
// hand-checked expectations. Also exercises the stop sentinel.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const arena_init           = M.cwrap("arena_init",           "number", ["number","number"]);
const arena_run_module     = M.cwrap("arena_run_module",     "number", ["string","string"]);
const arena_set_trace_mode = M.cwrap("arena_set_trace_mode", null,     ["number"]);
const arena_destroy        = M.cwrap("arena_destroy",        null,     []);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

const TRACE_OFF = 0, TRACE_SCAN = 1, TRACE_DRILL = 2;
const K_NAME = 0, K_FUNC_ENTER = 1, K_FUNC_EXIT = 2, K_LINE = 3, K_THROW = 4;

// ── Decode helpers ───────────────────────────────────────────────────
const dec = new TextDecoder();
function readU32(ptr)  { return M.HEAPU32[ptr >> 2]; }
function readU16(ptr)  { return M.HEAPU16[ptr >> 1]; }
function readStr(ptr, len) { return dec.decode(M.HEAPU8.subarray(ptr, ptr + len)); }

function decode(kind, ptr, len) {
    switch (kind) {
        case K_NAME: {
            const atom = readU32(ptr);
            const slen = readU16(ptr + 4);
            return { kind: "NAME", atom, name: readStr(ptr + 6, slen) };
        }
        case K_FUNC_ENTER:
            return { kind: "FUNC_ENTER",
                     name_atom: readU32(ptr), file_atom: readU32(ptr + 4),
                     line: readU32(ptr + 8) };
        case K_FUNC_EXIT:
            return { kind: "FUNC_EXIT" };
        case K_LINE:
            return { kind: "LINE", file_atom: readU32(ptr), line: readU32(ptr + 4) };
        case K_THROW: {
            const file_atom = readU32(ptr);
            const line = readU32(ptr + 4);
            const mlen = readU16(ptr + 8);
            return { kind: "THROW", file_atom, line, message: readStr(ptr + 10, mlen) };
        }
        default:
            return { kind: "?", raw_kind: kind };
    }
}

let events = [];
let stopAfter = -1;
M.host_trace = (kind, ptr, len) => {
    events.push(decode(kind, ptr, len));
    return stopAfter >= 0 && events.length >= stopAfter ? 1 : 0;
};

function resolveNames(evts) {
    // Replace atom ids with resolved name strings using NAME events.
    const map = new Map();
    const out = [];
    for (const e of evts) {
        if (e.kind === "NAME") { map.set(e.atom, e.name); continue; }
        const r = { ...e };
        if ("name_atom" in r) r.name = map.get(r.name_atom) ?? `<atom:${r.name_atom}>`;
        if ("file_atom" in r) r.file = map.get(r.file_atom) ?? `<atom:${r.file_atom}>`;
        delete r.name_atom; delete r.file_atom;
        out.push(r);
    }
    return out;
}

let passed = 0, total = 0;
function check(label, ok, ...rest) {
    total++;
    if (ok) passed++;
    else console.log("!! " + label, ...rest);
}

// ── Test 1: scan mode emits FUNC_ENTER/EXIT for nested calls ─────────
//
// Module top-level evaluates as an async function (QJS-ng wraps module
// bodies even when there's no top-level await). That means the module
// body suspends once and resumes once during evaluation, producing a
// SECOND ENTER/EXIT pair for the body in addition to the user calls.
// So for {module body, outer, inner} we expect 4 ENTERs and 4 EXITs.
events = []; stopAfter = -1;
arena_set_trace_mode(TRACE_SCAN);
{
    const src = `
        function inner() { return 7; }
        function outer() { return inner() + 1; }
        if (outer() !== 8) throw new Error("bad");
    `;
    const rc = arena_run_module("t1.js", src);
    check("scan: rc=0", rc === 0);

    const e = resolveNames(events);
    const enters = e.filter(x => x.kind === "FUNC_ENTER").length;
    const exits  = e.filter(x => x.kind === "FUNC_EXIT").length;
    check("scan: balanced enters/exits", enters === exits, `enters=${enters} exits=${exits}`);
    check("scan: no LINE events in scan mode",
          !e.some(x => x.kind === "LINE"));

    const enterNames = e.filter(x => x.kind === "FUNC_ENTER").map(x => x.name);
    check("scan: inner enter has name 'inner'", enterNames.includes("inner"), enterNames);
    check("scan: outer enter has name 'outer'", enterNames.includes("outer"), enterNames);
}

// ── Test 2: drill mode adds LINE events on source-line transitions ───
//
// QJS's pc2line table is sparse — not every source line gets an entry,
// so we don't see a strict {1,2,3,4} sequence. Instead assert that LINE
// events fire, multiple distinct lines show up, and consecutive
// duplicates from the same (file, line) get dedup'd (different files
// emitting the same line is allowed because each file's last-emitted
// state is per-frame conceptually — module body and user code can both
// be on "line 1" of their respective filename).
events = []; stopAfter = -1;
arena_set_trace_mode(TRACE_DRILL);
{
    const src = "let a = 1;\nlet b = 2;\nlet c = a + b;\nif (c !== 3) throw new Error('bad');\n";
    const rc = arena_run_module("t2.js", src);
    check("drill: rc=0", rc === 0);

    const e = resolveNames(events);
    const lineEvents = e.filter(x => x.kind === "LINE");
    check("drill: at least 2 LINE events", lineEvents.length >= 2, lineEvents.length);
    const distinct = new Set(lineEvents.map(x => `${x.file}:${x.line}`));
    check("drill: multiple distinct (file,line) seen", distinct.size >= 2, [...distinct]);
    // No two consecutive LINE events with same (file, line) WITHOUT
    // a FUNC_ENTER/EXIT in between. Same (file, line) emitted across
    // a frame boundary is intentional — a new physical frame restarts
    // the in-frame line tracker, and the first LINE in the new frame
    // should always fire even if it's at the same source position.
    let dedupOk = true;
    for (let i = 1; i < e.length; i++) {
        if (e[i].kind !== "LINE") continue;
        // Look back: find the most recent LINE event; if anything
        // FUNC_* sits between, the reset is legitimate.
        for (let j = i - 1; j >= 0; j--) {
            if (e[j].kind === "FUNC_ENTER" || e[j].kind === "FUNC_EXIT") break;
            if (e[j].kind !== "LINE") continue;
            if (e[j].file === e[i].file && e[j].line === e[i].line) dedupOk = false;
            break;
        }
    }
    check("drill: in-frame dedup per (file, line)", dedupOk, e);
}

// ── Test 3: THROW event fires at the throw site ──────────────────────
events = []; stopAfter = -1;
arena_set_trace_mode(TRACE_SCAN);
{
    const src = `
        function boom() { throw new Error("kaboom"); }
        try { boom(); } catch (e) { /* swallow */ }
    `;
    const rc = arena_run_module("t3.js", src);
    check("throw: rc=0 (caught)", rc === 0);

    const e = resolveNames(events);
    const throws = e.filter(x => x.kind === "THROW");
    check("throw: exactly one THROW event", throws.length === 1, throws);
    check("throw: message includes 'kaboom'",
          throws[0]?.message?.includes("kaboom"), throws[0]);
    check("throw: file is t3.js", throws[0]?.file === "t3.js", throws[0]);
}

// ── Test 4: host_trace returns 1 → execution stops cleanly ───────────
events = []; stopAfter = 3;   // halt after 3rd event
arena_set_trace_mode(TRACE_SCAN);
{
    const src = `
        function f1() { return 1; }
        function f2() { return 2; }
        function f3() { return 3; }
        function f4() { return 4; }
        f1(); f2(); f3(); f4();
        throw new Error("should not reach");
    `;
    const rc = arena_run_module("t4.js", src);
    check("stop: rc=0 (clean stop, not error)", rc === 0);
    check("stop: only 3 events collected", events.length === 3, events.length);
}

// ── Test 5: trace mode OFF → zero events ─────────────────────────────
events = []; stopAfter = -1;
arena_set_trace_mode(TRACE_OFF);
{
    const src = "function f() { return 42; } if (f() !== 42) throw new Error('bad');";
    const rc = arena_run_module("t5.js", src);
    check("off: rc=0", rc === 0);
    check("off: zero events", events.length === 0, events.length);
}

arena_destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
