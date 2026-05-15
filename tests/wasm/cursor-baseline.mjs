// Plain-execution baseline.
//
// Same tight loop as cursor-bench, but with trace mode OFF so we
// measure raw arenajs throughput — no host_trace callbacks, no event
// emission. Tells us how big N gets before just running the request
// crosses 10ms (and so produces a drill stream large enough to
// matter for replay-cursor sizing).

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const arena_init     = M.cwrap("arena_init",     "number", ["number","number"]);
const arena_run_mod  = M.cwrap("arena_run_module","number", ["string","string"]);
const set_trace_mode = M.cwrap("arena_set_trace_mode", null, ["number"]);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

set_trace_mode(0);                // OFF
M.tapes = {};
M.module_sources = {};

function build(N) {
    return `let sum = 0;
for (let i = 0; i < ${N}; i++) {
    sum += i;
    sum -= 1;
    sum ^= 7;
}
globalThis._ = sum;
`;
}

function time(N, reps = 5) {
    const src = build(N);
    // warm
    arena_run_mod("loop.js", src);
    const samples = [];
    for (let i = 0; i < reps; i++) {
        const t0 = performance.now();
        arena_run_mod("loop.js", src);
        samples.push(performance.now() - t0);
    }
    samples.sort((a, b) => a - b);
    return samples[Math.floor(samples.length / 2)]; // median
}

console.log(`${"N".padStart(11)}  ${"ms (median)".padStart(12)}  ${"M iters/s".padStart(12)}`);

const Ns = [1_000, 10_000, 100_000, 1_000_000, 10_000_000, 100_000_000];
for (const N of Ns) {
    const ms = time(N);
    const rate = N / ms / 1000; // millions of iters/sec
    console.log(`${N.toString().padStart(11)}  ${ms.toFixed(2).padStart(12)}` +
                `  ${rate.toFixed(1).padStart(12)}`);
}
