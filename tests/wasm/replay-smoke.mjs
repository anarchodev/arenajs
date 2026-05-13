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

// ── divergence detection: wrong key ──────────────────────────────────
setTapes({
    kv: [{ op: GET, outcome: OK, key: "expected", value: "x" }],
});
check("divergence on wrong key",
    run("kv.get wrong-key",
        "kv.get('unexpected'); 'should-not-reach'", true));

// ── exhausted tape ───────────────────────────────────────────────────
setTapes({ math_random: [{ value: 0.5 }] });
check("exhausted math_random",
    run("math_random exhausted",
        "Math.random(); Math.random(); 'should-not-reach'", true));

arena_destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
