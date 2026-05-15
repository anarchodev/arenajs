// Cursor benchmark.
//
// Walks every drill event from a tight N-iter loop, M events at a
// time, via the CursorEngine. Each page replays from start in scan
// mode then flips to drill at the anchor, so total work is
// approximately:
//
//   pages * cost_per_replay(N) = ceil(events / M) * cost_per_replay(N)
//
// Small M → many pages → quadratic-ish total cost; large M → one
// or two pages → linear in N. The bench prints µs/event for each
// combo so the work amplification from small page sizes is obvious.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";
import { CursorEngine } from "./cursor.mjs";

const Module = await getArenaJs();
const arena_init = Module.cwrap("arena_init", "number", ["number","number"]);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");
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

async function walkAll(replay, pageSize) {
    let cursor = eng.openCursor(replay, { kind: "scan", ordinal: 0 });
    let totalEvents = 0, pages = 0;
    const t0 = performance.now();
    while (cursor) {
        const p = await eng.drillNext(cursor, pageSize);
        totalEvents += p.events.length;
        pages++;
        cursor = p.next;
    }
    const t1 = performance.now();
    return { ms: t1 - t0, events: totalEvents, pages };
}

function fmt(n, w = 0, d = 0) {
    return n.toFixed(d).padStart(w);
}

const Ns = [100, 1000, 10000];
const Ms = [10, 100, 1000, 10000, 1_000_000];

for (const N of Ns) {
    const replay = buildReplay(N);

    const sct0 = performance.now();
    const idx = await eng.scanIndex(replay);
    const sct1 = performance.now();
    console.log(`\nN=${N}: scanIndex ${fmt(sct1-sct0, 6, 1)}ms` +
                `  records=${idx.length}`);

    // Warm + baseline: largest M is one-page, that's the lower bound
    // for per-event cost. Compare smaller M against it.
    const baseline = await walkAll(replay, Ms[Ms.length - 1]);
    const basePerEv = baseline.ms * 1000 / Math.max(baseline.events, 1);

    console.log(`  ${"page".padStart(8)}  ${"ms".padStart(8)}` +
                `  ${"events".padStart(8)}  ${"pages".padStart(6)}` +
                `  ${"µs/event".padStart(9)}  amp×`);
    for (const m of Ms) {
        const r = await walkAll(replay, m);
        const perEv = r.ms * 1000 / Math.max(r.events, 1);
        const amp = perEv / basePerEv;
        console.log(`  ${fmt(m, 8)}  ${fmt(r.ms, 8, 1)}` +
                    `  ${fmt(r.events, 8)}  ${fmt(r.pages, 6)}` +
                    `  ${fmt(perEv, 9, 2)}  ${amp.toFixed(1)}×`);
    }
}
