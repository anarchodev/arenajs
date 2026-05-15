// Cursor smoke test.
//
// Builds a CursorEngine over one long-lived arena, runs scanIndex
// against a small multi-function replay, then exercises drillNext
// against scan and line anchors with paging.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";
import { CursorEngine } from "./cursor.mjs";

const Module = await getArenaJs();
const arena_init = Module.cwrap("arena_init", "number", ["number","number"]);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

const eng = new CursorEngine(Module);

const replay = {
    entry: {
        name: "t.js",
        src: `
            function inner(x) { return x * 2; }
            function outer(y) {
                let a = inner(y);
                let b = inner(a);
                return b + 1;
            }
            let result = outer(5);
            if (result !== 21) throw new Error("bad: " + result);
        `,
    },
    tapes: {},
    module_sources: {},
};

let passed = 0, total = 0;
function check(label, ok, ...rest) {
    total++;
    if (ok) passed++; else console.log("!! " + label, ...rest);
}

// ── Test 1: scanIndex shape ──────────────────────────────────────────
const idx = await eng.scanIndex(replay);
check("scanIndex: non-empty", idx.length > 0, idx.length);
const enters = idx.filter(e => e.kind === "FUNC_ENTER");
const exits  = idx.filter(e => e.kind === "FUNC_EXIT");
check("scanIndex: balanced enter/exit", enters.length === exits.length,
      `enters=${enters.length} exits=${exits.length}`);
check("scanIndex: inner present", enters.some(e => e.name === "inner"));
check("scanIndex: outer present", enters.some(e => e.name === "outer"));
check("scanIndex: ordinals sequential",
      idx.every((e, i) => e.ordinal === i));
const outerEnter = enters.find(e => e.name === "outer");
const innerEnter = enters.find(e => e.name === "inner");
check("scanIndex: depth > 0", outerEnter.depth > 0, outerEnter);
check("scanIndex: inner deeper than outer",
      innerEnter.depth > outerEnter.depth,
      `inner=${innerEnter.depth} outer=${outerEnter.depth}`);

// ── Test 2: cache hit returns same array reference ───────────────────
const idx2 = await eng.scanIndex(replay);
check("scanIndex: cache reuses array", idx === idx2);

// ── Test 3: drillNext at a scan anchor pulls drill events ────────────
{
    const cur = eng.openCursor(replay, { kind: "scan", ordinal: outerEnter.ordinal });
    const page = await eng.drillNext(cur, 200);
    check("drill@scan: got events", page.events.length > 0);
    check("drill@scan: at least one LINE", page.events.some(e => e.kind === "LINE"));
    check("drill@scan: scanOrdinal >= anchor for all",
          page.events.every(e => e.scanOrdinal >= outerEnter.ordinal),
          page.events.slice(0, 3));
    check("drill@scan: next is null (ran to end)", page.next === null);
}

// ── Test 4: paging — small limit, then continue from page.next ───────
{
    const cur = eng.openCursor(replay, { kind: "scan", ordinal: outerEnter.ordinal });
    const a = await eng.drillNext(cur, 3);
    check("page1: exactly 3 events", a.events.length === 3, a.events.length);
    check("page1: next is non-null", a.next !== null);
    check("page1: next offset = 3",
          a.next && a.next.drillEventsAfterAnchor === 3);

    const b = await eng.drillNext(a.next, 3);
    check("page2: events present", b.events.length > 0);
    // Pages should not overlap — page2's first event differs from page1's first.
    check("page2: distinct from page1",
          JSON.stringify(b.events[0]) !== JSON.stringify(a.events[0]),
          a.events[0], b.events[0]);

    // Continue paging until we hit the end.
    let cursor = b.next;
    let safety = 100;
    while (cursor && safety-- > 0) {
        const p = await eng.drillNext(cursor, 50);
        cursor = p.next;
    }
    check("paging: terminates at end-of-replay", cursor === null);
}

// ── Test 5: line anchor — pick a known LINE event, seek there ────────
{
    // Find a LINE event in the full drill stream first.
    const full = await eng.drillNext(
        eng.openCursor(replay, { kind: "scan", ordinal: 0 }),
        10000);
    const lineEvents = full.events.filter(e => e.kind === "LINE");
    check("drill from origin: LINEs present", lineEvents.length > 0,
          full.events.length);

    if (lineEvents.length > 0) {
        // Pick a LINE event not at the very start, so afterScan can be 0
        // and we still test the "skip until match" path.
        const target = lineEvents[Math.floor(lineEvents.length / 2)];
        const cur = eng.openCursor(replay, {
            kind: "line",
            file: target.file,
            line: target.line,
            afterScan: 0,
        });
        const page = await eng.drillNext(cur, 50);
        check("line anchor: got events", page.events.length > 0);
        // First event past the anchor: must NOT be the anchor LINE itself
        // (page starts after the matching LINE). It must come from at
        // least as deep a scan position as the target.
        if (page.events.length > 0) {
            const first = page.events[0];
            check("line anchor: first event past anchor",
                  !(first.kind === "LINE" &&
                    first.file === target.file &&
                    first.line === target.line &&
                    first.scanOrdinal === target.scanOrdinal),
                  first, target);
        }
    }
}

// ── Test 6: invalid limit rejected ───────────────────────────────────
{
    let threw = false;
    try {
        await eng.drillNext(eng.openCursor(replay, { kind: "scan", ordinal: 0 }), 0);
    } catch (_) { threw = true; }
    check("drillNext: rejects limit=0", threw);
}

console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
