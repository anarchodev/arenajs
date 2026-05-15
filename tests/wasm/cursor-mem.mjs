// Memory footprint of a fully-materialised drill stream.
//
// For each replay size N, drills every event into a JS array in one
// fat page, then reports heap delta and bytes/event. Tells us whether
// the alternative architecture — "just drill the whole thing once and
// hold it" — is viable as the page count gets large.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";
import { CursorEngine } from "./cursor.mjs";

const Module = await getArenaJs();
if (Module.cwrap("arena_init","number",["number","number"])(8192, 8192) !== 0)
    throw new Error("arena_init failed");
const eng = new CursorEngine(Module);

function buildReplay(N) {
    return {
        entry: {
            name: "loop.js",
            src:
                `let sum = 0;\n` +
                `for (let i = 0; i < ${N}; i++) {\n` +
                `    sum += i;\n` +
                `    sum -= 1;\n` +
                `    sum ^= 7;\n` +
                `}\n` +
                `globalThis._ = sum;\n`,
        },
        tapes: {},
        module_sources: {},
    };
}

function mb(bytes) { return (bytes / (1024 * 1024)).toFixed(2); }
function gc() {
    if (typeof global.gc === "function") global.gc();
}

const Ns = [100, 1_000, 10_000, 100_000];

console.log(`${"N".padStart(9)}  ${"events".padStart(8)}` +
            `  ${"heapΔ".padStart(10)}  ${"B/event".padStart(8)}` +
            `  ${"drill ms".padStart(8)}`);

for (const N of Ns) {
    const replay = buildReplay(N);

    gc();
    const before = process.memoryUsage().heapUsed;

    const t0 = performance.now();
    const cur = eng.openCursor(replay, { kind: "scan", ordinal: 0 });
    const page = await eng.drillNext(cur, 100_000_000);
    const t1 = performance.now();

    // Hold a reference so GC doesn't reclaim before we measure.
    const events = page.events;

    const after = process.memoryUsage().heapUsed;
    const delta = after - before;
    const perEv = delta / Math.max(events.length, 1);

    console.log(`${N.toString().padStart(9)}  ${events.length.toString().padStart(8)}` +
                `  ${(mb(delta) + " MB").padStart(10)}  ${perEv.toFixed(0).padStart(8)}` +
                `  ${(t1 - t0).toFixed(0).padStart(8)}`);

    // Release events array before next iteration so heap deltas are clean.
    page.events = null;
}
