// arena_snapshot_here smoke test.
//
// Verifies that calling arena_snapshot_here() from inside a
// host_trace callback:
//   - fires the host_state JSON snapshot (same payload as host_trace=2)
//   - does NOT raise the stop sentinel — execution continues normally
//   - works from any trace-event kind (scan + drill)
//
// This is the engine touchpoint that unlocks materialise()-time
// variable snapshots and cluster-capable inspectAt later in the
// cursor module.

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const arena_init           = M.cwrap("arena_init",           "number", ["number","number"]);
const arena_run_module     = M.cwrap("arena_run_module",     "number", ["string","string"]);
const arena_set_trace_mode = M.cwrap("arena_set_trace_mode", null,     ["number"]);
const arena_snapshot_here  = M.cwrap("arena_snapshot_here",  "number", []);
if (arena_init(8192, 8192) !== 0) throw new Error("arena_init failed");

const TRACE_OFF = 0, TRACE_SCAN = 1, TRACE_DRILL = 2;
const K_NAME = 0, K_FUNC_ENTER = 1, K_FUNC_EXIT = 2, K_LINE = 3, K_THROW = 4;

const dec = new TextDecoder();
function readStr(ptr, len) { return dec.decode(M.HEAPU8.subarray(ptr, ptr + len)); }

let passed = 0, total = 0;
function check(label, ok, ...rest) {
    total++;
    if (ok) passed++; else console.log("!! " + label, ...rest);
}

// ── Test 1: snapshot_here from inside a LINE event ───────────────────
//
// Drill mode through a tight script; on the first LINE event seen
// inside the user code, ask for a snapshot. LINE events fire from
// the dispatch-loop-top hook after frame setup is complete, so the
// JS stack is fully linked. Verify (a) host_state received non-empty
// JSON, (b) the run completed (no stop sentinel), (c) total events
// show the run continued past the snapshot point.
{
    let snapshotCalls = 0;
    let snapshotJson = null;
    let eventCount = 0;
    let snapshotFired = false;
    let snapshotAtEvent = -1;

    M.host_state = (ptr, len) => {
        snapshotJson = readStr(ptr, len);
        snapshotCalls++;
    };
    M.host_trace = (kind) => {
        eventCount++;
        if (kind === K_LINE && !snapshotFired) {
            const rc = arena_snapshot_here();
            check("snapshot_here returns 0", rc === 0);
            snapshotFired = true;
            snapshotAtEvent = eventCount;
        }
        return 0;
    };

    arena_set_trace_mode(TRACE_DRILL);
    const src = `
        function inner(x) { return x * 2; }
        function outer(y) {
            let a = inner(y);
            let b = inner(a);
            return b + 1;
        }
        let result = outer(5);
        if (result !== 21) throw new Error("bad: " + result);
    `;
    const rc = arena_run_module("snap.js", src);
    arena_set_trace_mode(TRACE_OFF);

    check("test1: rc=0 (no stop)", rc === 0);
    check("test1: host_state fired exactly once", snapshotCalls === 1, snapshotCalls);
    check("test1: snapshot JSON non-empty", snapshotJson && snapshotJson.length > 0);
    check("test1: snapshot is a JSON array",
          snapshotJson && snapshotJson.startsWith("["));
    check("test1: execution continued past snapshot",
          eventCount > snapshotAtEvent, `events=${eventCount} snapAt=${snapshotAtEvent}`);

    if (snapshotJson) {
        const parsed = JSON.parse(snapshotJson);
        check("test1: parsed is array", Array.isArray(parsed));
        check("test1: at least one frame", parsed.length >= 1, parsed);
        if (parsed.length > 0) {
            check("test1: frame has func/file/line/vars",
                  "func" in parsed[0] && "file" in parsed[0] &&
                  "line" in parsed[0] && "vars" in parsed[0],
                  Object.keys(parsed[0]));
        }
    }

    M.host_state = null;
    M.host_trace = null;
}

// ── Test 2: many snapshots in one run, all continue execution ────────
{
    let snapshotCount = 0;
    let eventCount = 0;
    M.host_state = () => { snapshotCount++; };
    M.host_trace = () => {
        eventCount++;
        if (eventCount % 5 === 0) arena_snapshot_here();
        return 0;
    };

    arena_set_trace_mode(TRACE_DRILL);
    const src = `
        let s = 0;
        for (let i = 0; i < 30; i++) s += i;
        if (s !== 435) throw new Error("bad");
    `;
    const rc = arena_run_module("loop.js", src);
    arena_set_trace_mode(TRACE_OFF);

    check("test2: rc=0", rc === 0);
    check("test2: snapshots fired multiple times", snapshotCount >= 2, snapshotCount);
    check("test2: total events much greater than snapshots",
          eventCount > snapshotCount * 2,
          `events=${eventCount} snaps=${snapshotCount}`);
    M.host_state = null;
    M.host_trace = null;
}

// ── Test 3: snapshot_here outside an active trace event returns -1 ───
{
    arena_set_trace_mode(TRACE_OFF);
    const rc = arena_snapshot_here();
    check("test3: returns -1 when no active event", rc === -1, rc);
}

// ── Test 4: snapshot + host_trace=1 still produces a clean stop ──────
//
// Verify that snapshot_here doesn't somehow interfere with the
// stop sentinel: a host_trace that snapshots and then returns 1
// must still terminate cleanly.
{
    let snapshotted = false;
    let stopReturned = false;
    M.host_state = () => { snapshotted = true; };
    M.host_trace = (kind) => {
        if (kind === K_FUNC_ENTER && !snapshotted) {
            arena_snapshot_here();
            return 0;  // continue past snapshot
        }
        if (snapshotted && !stopReturned) {
            stopReturned = true;
            return 1;  // stop AFTER snapshot
        }
        return 0;
    };

    arena_set_trace_mode(TRACE_SCAN);
    const src = `
        function f() { return 1; }
        f(); f(); f();
    `;
    const rc = arena_run_module("stop.js", src);
    arena_set_trace_mode(TRACE_OFF);
    check("test4: rc=0 after snapshot+stop", rc === 0);
    check("test4: snapshot fired", snapshotted);
    check("test4: stop signalled", stopReturned);
    M.host_state = null;
    M.host_trace = null;
}

console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
