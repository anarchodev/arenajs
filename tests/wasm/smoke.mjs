// Tiny Node smoke test for qjs_arena_wasm.
// Run with: node tests/wasm/smoke.mjs (from repo root)

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const Module = await getArenaJs();

const arena_init    = Module.cwrap("arena_init",    "number", ["number", "number"]);
const arena_run     = Module.cwrap("arena_run",     "number", ["string"]);
const arena_destroy = Module.cwrap("arena_destroy", null,     []);

if (arena_init(4096, 4096) !== 0) {
    console.error("arena_init failed");
    process.exit(1);
}

const cases = [
    "1 + 2",
    "let xs = []; for (let i = 0; i < 5; i++) xs.push(i*i); xs.join(',')",
    "JSON.stringify({hello: 'world', n: 42})",
    "(() => { throw new Error('oops') })()",
    "Math.PI.toFixed(4)",
];

for (const src of cases) {
    console.log(`>>> ${src}`);
    const rc = arena_run(src);
    console.log(`    rc=${rc}`);
}

arena_destroy();
console.log("smoke ok");
