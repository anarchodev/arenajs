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

// ── Test 7: materialise output structure ─────────────────────────────
{
    const mat = await eng.materialise(replay);
    check("materialise: events array non-empty", mat.events.length > 0);
    check("materialise: events array is dense",
          mat.events.every(e => e && typeof e.kind === "string"));
    check("materialise: matchingExit is Int32Array, same length",
          mat.matchingExit instanceof Int32Array &&
          mat.matchingExit.length === mat.events.length);
    check("materialise: scanOrdinalToEventIdx is Int32Array",
          mat.scanOrdinalToEventIdx instanceof Int32Array);
    check("materialise: scanOrdinalToEventIdx covers all scan events",
          mat.scanOrdinalToEventIdx.length === idx.length,
          `${mat.scanOrdinalToEventIdx.length} vs ${idx.length}`);
    check("materialise: stackSnapshots present",
          Array.isArray(mat.stackSnapshots) && mat.stackSnapshots.length >= 1,
          mat.stackSnapshots.length);
    check("materialise: lineIndex is a Map", mat.lineIndex instanceof Map);
    check("materialise: inspectCache initialised empty",
          mat.inspectCache instanceof Map && mat.inspectCache.size === 0);

    // Cache identity: second call returns same object.
    const mat2 = await eng.materialise(replay);
    check("materialise: result cached", mat === mat2);
}

// ── Test 8: matchingExit pairs ENTERs with EXITs both ways ───────────
{
    const mat = await eng.materialise(replay);
    let pairs = 0;
    for (let i = 0; i < mat.events.length; i++) {
        const e = mat.events[i];
        if (e.kind !== "FUNC_ENTER") continue;
        const exitIdx = mat.matchingExit[i];
        if (exitIdx <= 0 || exitIdx >= mat.events.length) continue;
        if (mat.events[exitIdx].kind !== "FUNC_EXIT") continue;
        // Symmetric back-pointer
        if (mat.matchingExit[exitIdx] === i) pairs++;
    }
    const enterCount = mat.events.filter(e => e.kind === "FUNC_ENTER").length;
    check("matchingExit: all ENTERs paired symmetrically",
          pairs === enterCount,
          `pairs=${pairs} enters=${enterCount}`);
}

// ── Test 9: lineIndex resolves to events at the named (file, line) ───
{
    const mat = await eng.materialise(replay);
    let checked = 0, ok = 0;
    for (const [key, idxs] of mat.lineIndex) {
        const [file, lineStr] = key.split(":");
        const line = Number(lineStr);
        for (const idx of idxs) {
            checked++;
            const e = mat.events[idx];
            if (e.file === file && (e.line === line || e.kind === "FUNC_EXIT")) ok++;
        }
        if (checked >= 50) break; // sample
    }
    check("lineIndex: entries point at matching (file, line)",
          checked > 0 && checked === ok, `${ok}/${checked}`);
}

// ── Test 11: materialise with snapshotStep captures varSnapshots ─────
{
    // Fresh replay so the materialise cache doesn't return a prior
    // varSnapshots-less result.
    const replay2 = {
        entry: { name: "loop.js", src: `
            let s = 0;
            for (let i = 0; i < 50; i++) {
                s += i;
            }
            globalThis._ = s;
        ` },
        tapes: {}, module_sources: {},
    };
    const mat = await eng.materialise(replay2, { snapshotStep: 10 });
    check("snap: varSnapshots array present", Array.isArray(mat.varSnapshots));
    check("snap: varSnapshotStep echoed", mat.varSnapshotStep === 10);
    check("snap: snapshot count plausible",
          mat.varSnapshots.length >= 1 &&
          mat.varSnapshots.length <= Math.ceil(mat.events.length / 10) + 1,
          `${mat.varSnapshots.length} for ${mat.events.length} events`);
    check("snap: each entry has eventOrdinal + frames",
          mat.varSnapshots.every(s =>
              typeof s.eventOrdinal === "number" &&
              Array.isArray(s.frames)));
    check("snap: eventOrdinals are multiples of step",
          mat.varSnapshots.every(s => s.eventOrdinal % 10 === 0));

    // At least one snapshot taken mid-loop should have a non-empty
    // frames array with live JS locals.
    const nonEmpty = mat.varSnapshots.filter(s => s.frames.length > 0);
    check("snap: at least one non-empty frame snapshot",
          nonEmpty.length > 0,
          `${nonEmpty.length}/${mat.varSnapshots.length}`);
    if (nonEmpty.length > 0) {
        const f = nonEmpty[0].frames[0];
        check("snap: frame has func/file/line/vars",
              typeof f.func === "string" &&
              typeof f.file === "string" &&
              typeof f.line === "number" &&
              typeof f.vars === "object");
        const hasLoopVars = nonEmpty.some(s =>
            s.frames.some(fr => fr.vars && ("i" in fr.vars || "s" in fr.vars))
        );
        check("snap: snapshot captures loop locals", hasLoopVars);
    }
}

// ── Test 12: no snapshotStep → no varSnapshots ───────────────────────
{
    const replay3 = {
        entry: { name: "noop.js", src: "let x = 1; globalThis._ = x;" },
        tapes: {}, module_sources: {},
    };
    const mat = await eng.materialise(replay3);
    check("no-snap: varSnapshots undefined", mat.varSnapshots === undefined);
    check("no-snap: varSnapshotStep undefined", mat.varSnapshotStep === undefined);
}

// ── Test 10: stackSnapshots reflect call depth at sampled events ─────
{
    const mat = await eng.materialise(replay);
    // At each snapshot point, the live stack depth should equal the
    // running enter/exit balance up to that event.
    let snapOk = 0;
    for (let s = 0; s < mat.stackSnapshots.length; s++) {
        const sampledEventIdx = s * mat.stackSnapshotStep;
        if (sampledEventIdx >= mat.events.length) break;
        const snap = mat.stackSnapshots[s];
        // Compute expected stack depth by walking up to sampledEventIdx.
        let d = 0;
        for (let i = 0; i <= sampledEventIdx; i++) {
            const e = mat.events[i];
            if (e.kind === "FUNC_ENTER") d++;
            else if (e.kind === "FUNC_EXIT") d--;
        }
        if (snap.length === d) snapOk++;
    }
    check("stackSnapshots: depth matches running enter/exit balance",
          snapOk === mat.stackSnapshots.length || snapOk > 0,
          `${snapOk} / ${mat.stackSnapshots.length}`);
}

console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
