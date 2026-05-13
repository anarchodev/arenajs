// End-to-end wire-format compatibility check.
//
// Path under test:
//   rove Zig tape recorder → RTAP bytes → JS parseTapeBlob →
//   buildTapesFromBlobs → Module.tapes → WASM bindings → handler eval
//
// We don't actually run a rove worker; we synthesise the bytes via
// rtap.mjs's serializeTape, which mirrors src/tape/root.zig's
// encoding rule-for-rule. If anything diverges from what rove emits,
// the round-trip parse here will hide it — but if rtap.mjs ever
// becomes the lift target in rove web/, the same code will be on
// both sides of the wire, so the round-trip stops being a
// self-consistency check and starts being the real contract.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";
import {
    serializeTape, parseTapeBlob, buildTapesFromBlobs,
    CHANNEL_KV, CHANNEL_DATE, CHANNEL_MATH_RANDOM, CHANNEL_CRYPTO_RANDOM,
    KV_OP_GET, KV_OP_SET, KV_OP_DELETE, KV_OP_PREFIX,
    KV_OUTCOME_OK, KV_OUTCOME_NOT_FOUND,
} from "./rtap.mjs";

const M = await getArenaJs();
const arena_init    = M.cwrap("arena_init",    "number", ["number","number"]);
const arena_run     = M.cwrap("arena_run",     "number", ["string"]);
const arena_destroy = M.cwrap("arena_destroy", null,     []);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

let passed = 0, total = 0;
function check(label, ok, ...rest) {
    total++;
    if (ok) passed++;
    else console.log("!! " + label, ...rest);
}

// ── Round-trip: serialize then parse, byte-shape checks ──────────────
{
    const blob = serializeTape(CHANNEL_KV, [
        { op: KV_OP_GET, outcome: KV_OUTCOME_OK,        key: "hits",    value: "42" },
        { op: KV_OP_GET, outcome: KV_OUTCOME_NOT_FOUND, key: "missing", value: "" },
        { op: KV_OP_SET, outcome: KV_OUTCOME_OK,        key: "name",    value: "rove" },
        { op: KV_OP_DELETE, outcome: KV_OUTCOME_OK,     key: "name",    value: "" },
        { op: KV_OP_PREFIX, outcome: KV_OUTCOME_OK,
          key: "score/", cursor: "", limit: 100,
          results: [
              { key: "score/alice", value: "10" },
              { key: "score/bob",   value: "7"  },
          ] },
    ]);
    // Header byte checks — fixed-position, easy to confirm.
    const v = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
    check("kv: magic = 'RTAP'",   v.getUint32(0) === 0x52544150, v.getUint32(0).toString(16));
    check("kv: version = 1",       v.getUint16(4) === 1);
    check("kv: channel = 0",        v.getUint16(6) === 0);
    check("kv: entry count = 5",    v.getUint32(8) === 5);

    const { entries } = parseTapeBlob(blob);
    check("kv: 5 entries parsed",  entries.length === 5);
    check("kv: entry[0] hits=42",  entries[0].key === "hits" && entries[0].value === "42");
    check("kv: entry[1] not_found",entries[1].outcome === KV_OUTCOME_NOT_FOUND);
    check("kv: entry[3] delete",   entries[3].op === KV_OP_DELETE);
    check("kv: entry[4] prefix",   entries[4].op === KV_OP_PREFIX && entries[4].results.length === 2);
    check("kv: prefix row[0] value=10", entries[4].results[0].value === "10");
}

// ── Date / math / crypto round-trip ──────────────────────────────────
{
    const dateBlob = serializeTape(CHANNEL_DATE, [
        { ms: 1715000000000 }, { ms: 0 }, { ms: -1 },
    ]);
    const date = parseTapeBlob(dateBlob).entries;
    check("date: 1715000000000", date[0].ms === 1715000000000);
    check("date: 0",             date[1].ms === 0);
    check("date: -1",            date[2].ms === -1);

    const mrBlob = serializeTape(CHANNEL_MATH_RANDOM, [
        { value: 0.123 }, { value: 0.5 }, { value: Math.PI },
    ]);
    const mr = parseTapeBlob(mrBlob).entries;
    check("math_random: bit-exact 0.123", mr[0].value === 0.123);
    check("math_random: bit-exact PI",    mr[2].value === Math.PI);

    const crBlob = serializeTape(CHANNEL_CRYPTO_RANDOM, [
        { bytes: new Uint8Array([0xde, 0xad, 0xbe, 0xef]) },
        { bytes: new Uint8Array(16).fill(0x42) },
    ]);
    const cr = parseTapeBlob(crBlob).entries;
    check("crypto: 4-byte payload",  cr[0].bytes.length === 4 && cr[0].bytes[0] === 0xde);
    check("crypto: 16-byte payload", cr[1].bytes.length === 16 && cr[1].bytes[15] === 0x42);
}

// ── End-to-end: bytes -> Module.tapes -> WASM handler ────────────────
//
// Build a multi-channel tape bundle (the shape rove's log records
// store), parse them through the bridge, install on Module, and run
// a handler that exercises every channel.
{
    const blobs = {
        kv: serializeTape(CHANNEL_KV, [
            { op: KV_OP_GET, outcome: KV_OUTCOME_OK, key: "counter", value: "10" },
        ]),
        date: serializeTape(CHANNEL_DATE, [
            { ms: 1715000000000 },
        ]),
        math_random: serializeTape(CHANNEL_MATH_RANDOM, [
            { value: 0.5 },
        ]),
        crypto_random: serializeTape(CHANNEL_CRYPTO_RANDOM, [
            { bytes: new Uint8Array([0xab, 0xcd]) },
        ]),
    };

    M.tapes = buildTapesFromBlobs(blobs);
    check("bridge: kv length 1",  M.tapes.kv.length === 1);
    check("bridge: kv key matches", M.tapes.kv[0].key === "counter");
    check("bridge: date ms",        M.tapes.date[0].ms === 1715000000000);

    const src = `
        const v = kv.get("counter");
        const d = Date.now();
        const r = Math.random();
        const b = crypto.randomBytes(2);
        const ok =
            v === "10" &&
            d === 1715000000000 &&
            r === 0.5 &&
            b[0] === 0xab && b[1] === 0xcd;
        if (!ok) throw new Error("bridge mismatch: " + JSON.stringify({v,d,r,b:[...b]}));
        "ok"
    `;
    const rc = arena_run(src);
    check("e2e: handler ran without divergence", rc === 0);
}

arena_destroy();
console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
