// Cursor UI-simulation bench.
//
// Walks the cursor module through the access patterns the rove
// scrubber will use against various trace shapes:
//
//   trace shapes: tight arithmetic loop, call-heavy, deep recursion,
//                 mixed (calls + throws + branching)
//   UI scenarios: materialise (no snaps),
//                 materialise + per-pixel varSnapshots,
//                 scrubber drag (60 frames of array indexing),
//                 arrow-key fine-stepping (initial inspect + cached),
//                 click-to-line jump (lineIndex lookup),
//                 heap delta per shape.
//
// Run with `node --expose-gc tests/wasm/cursor-ui-bench.mjs` for the
// most accurate heap deltas; without --expose-gc the heap numbers
// include retained junk from prior iterations.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";
import { CursorEngine } from "./cursor.mjs";

// 16 MB request arena. emit_state used to build a JSValue tree per
// snapshot (frame objects, var maps) into the bump arena, which never
// reclaims mid-run — dense cadence × deep stacks needed 128 MB+ and
// still OOM'd the deep-recursion shape. emit_state now serializes the
// stack walk directly to a libc-malloc'd byte buffer (which DOES
// reclaim), so the only arena cost is transient per-value
// stringification of complex values. Deep recursion N=800 now fits
// comfortably in 8 MB; 16 MB is generous headroom for the bench.
const Module = await getArenaJs();
if (Module.cwrap("arena_init","number",["number","number"])(8192, 16384) !== 0)
    throw new Error("arena_init failed");
const eng = new CursorEngine(Module);

function gc() { if (typeof global.gc === "function") global.gc(); }
function heap() { return process.memoryUsage().heapUsed; }
function fmt(n, w = 6, d = 0) { return n.toFixed(d).padStart(w); }
function mb(b) { return (b / (1024 * 1024)).toFixed(2) + " MB"; }

// ── Trace shape generators ───────────────────────────────────────────

function tightLoop(N) {
    return `let s = 0;
for (let i = 0; i < ${N}; i++) { s += i; s -= 1; s ^= 7; }
globalThis._ = s;`;
}

function callHeavy(N) {
    return `function inner(x) { return x + 1; }
function outer(y) { return inner(y) * 2; }
let s = 0;
for (let i = 0; i < ${N}; i++) s += outer(i);
globalThis._ = s;`;
}

function deepRecursion(N) {
    return `function rec(n) {
    if (n <= 0) return 0;
    return rec(n - 1) + 1;
}
globalThis._ = rec(${N});`;
}

function mixed(N) {
    return `function step(i) {
    if (i % 7 === 0) return i * 2;
    if (i % 13 === 0) return i - 1;
    return i + 3;
}
let s = 0;
for (let i = 0; i < ${N}; i++) {
    try {
        s += step(i);
        if (s > ${N} * 100) throw new Error("guard");
    } catch (e) { s = 0; }
}
globalThis._ = s;`;
}

const shapes = [
    { name: "tight loop",     gen: tightLoop,     N: 1000 },
    { name: "call-heavy",     gen: callHeavy,     N: 500  },
    { name: "deep recursion", gen: deepRecursion, N: 200  },
    { name: "mixed",          gen: mixed,         N: 500  },
];

function makeReplay(src) {
    return { entry: { name: "bench.js", src }, tapes: {}, module_sources: {} };
}

// Per-shape rows for the final summary table.
const summary = [];

for (const shape of shapes) {
    console.log(`\n── ${shape.name} (N=${shape.N}) ────────────────────────────`);
    const src = shape.gen(shape.N);

    // 1. materialise (no varSnapshots)
    const replayA = makeReplay(src);
    gc();
    const h0 = heap();
    let t0 = performance.now();
    const mat = await eng.materialise(replayA);
    let t1 = performance.now();
    const matMs = t1 - t0;
    const h1 = heap();
    const heapPlain = h1 - h0;
    const evCount = mat.events.length;
    const lineCount = mat.events.filter(e => e.kind === "LINE").length;
    const scanCount = mat.scanOrdinalToEventIdx.length;
    console.log(`materialise (plain):       ${fmt(matMs, 7, 1)} ms` +
                `  events=${evCount}  lines=${lineCount}  scans=${scanCount}` +
                `  heap+=${mb(heapPlain).padStart(8)}`);

    // 2. materialise targeting ~800 varSnapshots. Two-pass internally:
    // pass 1 counts events, pass 2 captures variable snapshots at the
    // step that yields the target. Step is reported in output.
    const replayB = makeReplay(src);
    gc();
    const h2 = heap();
    t0 = performance.now();
    const matSnap = await eng.materialise(replayB, { targetSnapshots: 800 });
    t1 = performance.now();
    const snapMs = t1 - t0;
    const h3 = heap();
    const heapWithSnaps = h3 - h2;
    const snapCount = matSnap.varSnapshots?.length ?? 0;
    const step800 = matSnap.varSnapshotStep ?? 0;
    console.log(`materialise + 800 snaps:   ${fmt(snapMs, 7, 1)} ms` +
                `  varSnaps=${snapCount}  step=${step800}` +
                `  heap+=${mb(heapWithSnaps).padStart(8)}`);

    // 3. Scrubber drag: 60 random-access frames into the materialised data
    const frames = 60;
    t0 = performance.now();
    let _sink = 0;
    for (let f = 0; f < frames; f++) {
        const pct = f / frames;
        const targetEv = Math.min(evCount - 1, Math.floor(pct * evCount));
        const ev = matSnap.events[targetEv];
        const stackS = Math.floor(targetEv / matSnap.stackSnapshotStep);
        const stack = matSnap.stackSnapshots[Math.min(stackS, matSnap.stackSnapshots.length - 1)];
        const varIdx = Math.min(snapCount - 1, Math.floor(targetEv / step800));
        const snap = matSnap.varSnapshots?.[varIdx];
        _sink += (ev?.line ?? 0) + (stack?.length ?? 0) + (snap?.frames?.length ?? 0);
    }
    t1 = performance.now();
    const dragMs = t1 - t0;
    const perFrameUs = (dragMs * 1000) / frames;
    console.log(`scrubber drag 60 frames:   ${fmt(dragMs, 7, 2)} ms` +
                `  ${fmt(perFrameUs, 6, 1)} µs/frame`);

    // 4. Arrow-key fine-step: one cluster fetch, then ten cached steps
    const mid = Math.floor(evCount / 2);
    t0 = performance.now();
    await eng.inspectAt(matSnap, mid, { cluster: 5 });
    const arrowT1 = performance.now();
    for (let k = -5; k <= 4; k++) {
        await eng.inspectAt(matSnap, mid + k, { cluster: 0 });
    }
    t1 = performance.now();
    console.log(`arrow-step: cluster fetch ${fmt(arrowT1-t0, 7, 2)} ms` +
                `, 10 cached ${fmt(t1-arrowT1, 7, 2)} ms ` +
                `(${fmt(((t1-arrowT1)*1000)/10, 5, 1)} µs/step)`);

    // 5. Click-to-line jump: 200 lineIndex lookups
    const lineKeys = [...matSnap.lineIndex.keys()];
    if (lineKeys.length > 0) {
        t0 = performance.now();
        const N = 200;
        let hits = 0;
        for (let i = 0; i < N; i++) {
            const k = lineKeys[i % lineKeys.length];
            const idxs = matSnap.lineIndex.get(k);
            if (idxs && idxs.length > 0) hits++;
        }
        t1 = performance.now();
        console.log(`click-jump 200×:           ${fmt(t1-t0, 7, 2)} ms` +
                    `  (${fmt(((t1-t0)*1000)/N, 5, 2)} µs/lookup)`);
    }

    summary.push({
        shape: shape.name,
        events: evCount,
        plainMs: matMs,
        snapMs: snapMs,
        snapCount,
        heapPlainMB: heapPlain / (1024*1024),
        heapSnapsMB: heapWithSnaps / (1024*1024),
        perFrameUs,
    });
}

// ── Summary table ────────────────────────────────────────────────────
console.log(`\n── Summary ──────────────────────────────────────────────────`);
console.log(`${"shape".padEnd(18)}  ${"events".padStart(8)}  ${"matMs".padStart(8)}` +
            `  ${"+snapsMs".padStart(8)}  ${"snaps".padStart(6)}` +
            `  ${"heap(plain)".padStart(11)}  ${"heap(+snaps)".padStart(12)}` +
            `  ${"frame µs".padStart(8)}`);
for (const s of summary) {
    console.log(`${s.shape.padEnd(18)}  ${fmt(s.events, 8)}  ${fmt(s.plainMs, 8, 1)}` +
                `  ${fmt(s.snapMs, 8, 1)}  ${fmt(s.snapCount, 6)}` +
                `  ${fmt(s.heapPlainMB, 9, 2)} MB  ${fmt(s.heapSnapsMB, 10, 2)} MB` +
                `  ${fmt(s.perFrameUs, 8, 1)}`);
}
