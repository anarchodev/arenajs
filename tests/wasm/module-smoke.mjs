// Module-loader smoke test.
//
// Each test sets up Module.tapes.module (the recording of what
// specifier resolved to what hash, in import order) and
// Module.module_sources (the path-keyed source bundle the replay
// shell would build from the deployment). arena_run_module loads the
// entry source directly; subsequent imports go through the replay
// module loader, which consumes one tape entry per import.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const Module = await getArenaJs();
const arena_init        = Module.cwrap("arena_init",        "number", ["number","number"]);
const arena_run         = Module.cwrap("arena_run",         "number", ["string"]);
const arena_run_module  = Module.cwrap("arena_run_module",  "number", ["string","string"]);
const arena_destroy     = Module.cwrap("arena_destroy",     null,     []);

if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

let passed = 0, total = 0;
function check(label, ok) { total++; if (ok) passed++; else console.log(`!! ${label}`); }
function setTapes(tapes) {
    for (const k of Object.keys(tapes)) tapes[k]._cursor = 0;
    Module.tapes = tapes;
}

// All assertions live inside the module body — a thrown exception
// propagates back as a non-zero rc. Cross-call globalThis observation
// won't work because `JS_ResetRequestArena` reclaims the request-arena
// pages the global-object's mutated shape table lives in.

// ── Case 1: entry imports one helper module ──────────────────────────
setTapes({
    module: [{ specifier: "helper.js", source_hash_hex: "deadbeef" }],
});
Module.module_sources = {
    "helper.js": "export const greet = (n) => 'hi ' + n;",
};
{
    const entry = `
        import { greet } from "helper.js";
        const out = greet("world");
        if (out !== "hi world") throw new Error("bad: " + out);
    `;
    check("entry imports one helper", arena_run_module("entry1.js", entry) === 0);
}

// ── Case 2: transitive imports (entry → mid → leaf) ──────────────────
setTapes({
    module: [
        // QJS resolves imports depth-first; entry imports "mid" first,
        // mid then imports "leaf" before we return to the entry.
        { specifier: "mid.js",  source_hash_hex: "aaa" },
        { specifier: "leaf.js", source_hash_hex: "bbb" },
    ],
});
Module.module_sources = {
    "mid.js":  "import { x } from 'leaf.js'; export const y = x * 2;",
    "leaf.js": "export const x = 21;",
};
{
    const entry = `
        import { y } from "mid.js";
        if (y !== 42) throw new Error("bad: " + y);
    `;
    check("transitive imports", arena_run_module("entry2.js", entry) === 0);
}

// ── Case 3: divergence on wrong specifier ────────────────────────────
setTapes({
    module: [{ specifier: "expected.js", source_hash_hex: "x" }],
});
Module.module_sources = { "actual.js": "export const v = 1;" };
{
    const entry = `import { v } from "actual.js"; globalThis._x = v;`;
    const rc = arena_run_module("entry3.js", entry);
    check("divergence detected on wrong specifier", rc !== 0);
}

// ── Case 4: relative-path resolution via QJS default normalize ───────
//
// The default normalize handles "./foo" against the importer's name.
// Entry "lib/index.js" imports "./util.js" → QJS asks the loader for
// "lib/util.js". The tape's recorded specifier must match that
// resolved form.
setTapes({
    module: [{ specifier: "lib/util.js", source_hash_hex: "c" }],
});
Module.module_sources = {
    "lib/util.js": "export const k = 7;",
};
{
    const entry = `
        import { k } from "./util.js";
        if (k !== 7) throw new Error("bad: " + k);
        globalThis._result4 = k;
    `;
    const rc = arena_run_module("lib/index.js", entry);
    check("relative-path normalize", rc === 0);
}

arena_destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
