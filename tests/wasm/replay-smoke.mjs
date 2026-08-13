// Replay-bindings smoke test.
// Builds parsed-tape arrays directly (skipping the .RTAP wire format —
// that lives in rove's app.js; arenajs cares only about the in-memory
// shape), seeds them into Module.tapes, and runs handler scripts that
// exercise each binding.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const Module = await getArenaJs();
const arena_init    = Module.cwrap("arena_init",    "number", ["number", "number"]);
const arena_run     = Module.cwrap("arena_run",     "number", ["string"]);
const arena_destroy = Module.cwrap("arena_destroy", null,     []);

if (arena_init(4096, 4096) !== 0) throw new Error("arena_init failed");

// KV outcome / op constants mirror src/tape/root.zig
const OK = 0, NOT_FOUND = 1, ERR = 2;
const GET = 0, SET = 1, DEL = 2, PREFIX = 3;

function setTapes(tapes) {
    // Reset cursors on every run so each test starts fresh.
    for (const k of Object.keys(tapes)) tapes[k]._cursor = 0;
    Module.tapes = tapes;
    // The kv/module channels resolve BY KEY through a lazily-built index
    // (_arena_index_inputs), built once per Module. Real embedders boot a
    // fresh Module per record; this harness reuses one, so the index must
    // be invalidated or every setTapes after the first is invisible.
    Module._inputIndex = null;
}

function run(label, src, expectFail = false) {
    console.log(`── ${label}`);
    const rc = arena_run(src);
    const ok = expectFail ? rc !== 0 : rc === 0;
    console.log(`   ${ok ? "OK" : "FAIL"} (rc=${rc})`);
    return ok;
}

let passed = 0, total = 0;
function check(label, ok) { total++; if (ok) passed++; else console.log(`!! ${label}`); }

// ── Math.random ──────────────────────────────────────────────────────
setTapes({
    math_random: [{ value: 0.123 }, { value: 0.456 }, { value: 0.789 }],
});
check("math_random sequence",
    run("math_random",
        "const a = Math.random(); const b = Math.random(); const c = Math.random(); " +
        "if (a !== 0.123 || b !== 0.456 || c !== 0.789) throw new Error('mismatch'); 'ok'"));

// ── Date.now ─────────────────────────────────────────────────────────
setTapes({ date: [{ ms: 1715000000000 }, { ms: 1715000000050 }] });
check("date sequence",
    run("date",
        "const a = Date.now(); const b = Date.now(); " +
        "if (a !== 1715000000000 || b !== 1715000000050) throw new Error('mismatch:' + a + ',' + b); 'ok'"));

// ── crypto.randomBytes ───────────────────────────────────────────────
setTapes({
    crypto_random: [{ bytes: new Uint8Array([0xde, 0xad, 0xbe, 0xef]) }],
});
check("crypto.randomBytes",
    run("crypto.randomBytes",
        "const b = crypto.randomBytes(4); " +
        "if (b.length !== 4 || b[0] !== 0xde || b[3] !== 0xef) throw new Error('mismatch:' + b); 'ok'"));

// ── crypto.getRandomValues ───────────────────────────────────────────
setTapes({
    crypto_random: [{ bytes: new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]) }],
});
check("crypto.getRandomValues",
    run("crypto.getRandomValues",
        "const ta = new Uint8Array(8); crypto.getRandomValues(ta); " +
        "if (ta[0] !== 1 || ta[7] !== 8) throw new Error('mismatch:' + ta); 'ok'"));

// ── crypto.randomUUID ────────────────────────────────────────────────
setTapes({
    crypto_random: [{ bytes: new Uint8Array(16).fill(0x42) }],
});
check("crypto.randomUUID",
    run("crypto.randomUUID",
        "const u = crypto.randomUUID(); " +
        "if (u !== '42424242-4242-4242-4242-424242424242') throw new Error('got:' + u); 'ok'"));

// ── kv.get with all three outcomes ───────────────────────────────────
setTapes({
    kv: [
        { op: GET, outcome: OK,        key: "hits",    value: "42" },
        { op: GET, outcome: NOT_FOUND, key: "missing", value: "" },
        { op: GET, outcome: ERR,       key: "broken",  value: "" },
    ],
});
check("kv.get outcomes",
    run("kv.get",
        "const a = kv.get('hits');" +
        "const b = kv.get('missing');" +
        "let threw = false; try { kv.get('broken'); } catch (e) { threw = true; }" +
        "if (a !== '42' || b !== null || !threw) throw new Error('kv.get mismatch ' + JSON.stringify({a,b,threw})); 'ok'"));

// ── kv.set / kv.delete ───────────────────────────────────────────────
setTapes({
    kv: [
        { op: SET, outcome: OK, key: "name", value: "rove" },
        { op: DEL, outcome: OK, key: "name", value: "" },
    ],
});
check("kv.set + kv.delete",
    run("kv.set/delete",
        "kv.set('name', 'rove'); kv.delete('name'); 'ok'"));

// ── kv.prefix ────────────────────────────────────────────────────────
setTapes({
    kv: [{
        op: PREFIX, outcome: OK,
        key: "score/", cursor: "", limit: 100,
        results: [
            { key: "score/alice", value: "10" },
            { key: "score/bob",   value: "7"  },
            { key: "score/carol", value: "13" },
        ],
    }],
});
check("kv.prefix",
    run("kv.prefix",
        "const rows = kv.prefix('score/', { limit: 100 }); " +
        "if (rows.length !== 3 || rows[0].key !== 'score/alice' || rows[2].value !== '13') " +
        "  throw new Error('mismatch:' + JSON.stringify(rows)); 'ok'"));

// ── kv.prefix merges the write overlay (read-your-writes) ───────────
// The page is reconstructed like the embedder's native host: recorded
// rows ∪ same-run writes, tombstones removed, sorted. A scan that
// ignored the overlay would show a handler its pre-write world — the
// exact class of engine divergence the embedder's conformance suite
// compares for (rove#517).
Module._kvOverlay = new Map();
setTapes({
    kv: [{
        op: PREFIX, outcome: OK,
        key: "score/", cursor: "", limit: 100,
        results: [
            { key: "score/alice", value: "10" },
            { key: "score/bob",   value: "7"  },
        ],
    }],
});
check("kv.prefix merges same-run writes",
    run("kv.prefix overlay",
        "kv.set('score/zed', '1'); kv.delete('score/alice'); kv.set('score/bob', '9'); " +
        "const rows = kv.prefix('score/', null, 100); " +
        "if (rows.length !== 2 || rows[0].key !== 'score/bob' || rows[0].value !== '9' " +
        "    || rows[1].key !== 'score/zed') " +
        "  throw new Error('mismatch:' + JSON.stringify(rows)); 'ok'"));

// ── kv.prefix with no recorded scan ──────────────────────────────────
// An authored world seeds the overlay directly and has no tape; an
// empty match is a legitimate answer, never a divergence — the native
// host never holes a prefix scan.
Module._kvOverlay = new Map([["s/a", "1"], ["s/b", "2"], ["t/x", "9"]]);
setTapes({ kv: [] });
check("kv.prefix untaped scans the overlay",
    run("kv.prefix authored",
        "const rows = kv.prefix('s/', null, 100); " +
        "const none = kv.prefix('none/', null, 100); " +
        "if (rows.length !== 2 || rows[0].key !== 's/a' || rows[1].value !== '2' " +
        "    || none.length !== 0) " +
        "  throw new Error('mismatch:' + JSON.stringify({rows, none})); 'ok'"));
Module._kvOverlay = new Map();

// ── divergence: the poison model (rove#510) ──────────────────────────
// An off-tape read is ABSENT, never a throw — nothing a handler can
// catch. Authored mode: plain absent. Captured mode: absent + a
// host-side verdict (module memory, unreachable from VM JS) that the
// interrupt BRAKES on — proven with an infinite loop that must die.
setTapes({
    kv: [{ op: GET, outcome: OK, key: "expected", value: "x" }],
});
Module._kvOverlay = new Map();
check("authored off-tape read is absent (no throw)",
    run("kv.get wrong-key authored",
        "if (kv.get('unexpected') !== null) throw new Error('not absent'); 'ok'"));
check("captured off-tape read poisons + the brake fires",
    run("kv.get wrong-key captured",
        "globalThis.__rove_captured = true; " +
        "if (kv.get('unexpected') !== null) throw new Error('not absent'); " +
        "const d = __rove_divergence(); " +
        "if (!d || d.indexOf('unexpected') < 0) throw new Error('no verdict: ' + d); " +
        "for(;;){}", true));

// ── exhausted tape ───────────────────────────────────────────────────
setTapes({ math_random: [{ value: 0.5 }] });
check("exhausted math_random",
    run("math_random exhausted",
        "Math.random(); Math.random(); 'should-not-reach'", true));

arena_destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
