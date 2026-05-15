// OOM-signaling smoke test.
//
// The product embedding arenajs must be able to tell three outcomes
// apart from arena_run_module's return code:
//   ARENA_RC_OK     0   success / clean stop
//   ARENA_RC_ERROR -1   JS code threw (user error)
//   ARENA_RC_OOM   -2   request arena exhausted (capacity)
// plus query arena_oom_{hit,requested,used,limit} for an actionable
// message. The OOM record is per-request (cleared on the next run).

import getArenaJs from "../../build-wasm/qjs_arena_wasm.js";

const M = await getArenaJs();
const arena_init       = M.cwrap("arena_init",       "number", ["number","number"]);
const arena_run_module = M.cwrap("arena_run_module", "number", ["string","string"]);
const oom_hit          = M.cwrap("arena_oom_hit",       "number", []);
const oom_requested    = M.cwrap("arena_oom_requested", "number", []);
const oom_used         = M.cwrap("arena_oom_used",      "number", []);
const oom_limit        = M.cwrap("arena_oom_limit",     "number", []);

// Small request arena so a modest allocation loop exhausts it.
if (arena_init(8192, 256) !== 0) throw new Error("arena_init failed");
M.tapes = {};
M.module_sources = {};

const RC_OK = 0, RC_ERROR = -1, RC_OOM = -2;

let passed = 0, total = 0;
function check(label, ok, ...rest) {
    total++;
    if (ok) passed++; else console.log("!! " + label, ...rest);
}

// ── Success ──────────────────────────────────────────────────────────
{
    const rc = arena_run_module("ok.js", "globalThis._ = 1 + 1;");
    check("success: rc=OK", rc === RC_OK, rc);
    check("success: oom_hit=0", oom_hit() === 0);
}

// ── JS throw is a user error, NOT OOM ────────────────────────────────
{
    const rc = arena_run_module("throw.js", "throw new Error('boom');");
    check("throw: rc=ERROR", rc === RC_ERROR, rc);
    check("throw: oom_hit=0 (not misclassified)", oom_hit() === 0);
}

// ── Arena exhaustion → distinct OOM signal with numbers ──────────────
{
    const rc = arena_run_module("oom.js",
        "let a=[]; for (let i=0;i<100000;i++) a.push({x:i,y:i*2,s:'pad'+i}); globalThis._=a.length;");
    check("oom: rc=OOM (not generic ERROR)", rc === RC_OOM, rc);
    check("oom: oom_hit=1", oom_hit() === 1);
    const req = oom_requested(), used = oom_used(), lim = oom_limit();
    check("oom: requested > 0", req > 0, req);
    check("oom: used > 0", used > 0, used);
    check("oom: limit > 0", lim > 0, lim);
    check("oom: used <= limit", used <= lim, `${used} / ${lim}`);
    check("oom: used near limit (genuinely full)", used > lim * 0.5,
          `${used} / ${lim}`);
}

// ── OOM record is per-request: a clean run after OOM clears it ───────
{
    const rc = arena_run_module("recover.js", "globalThis._ = 42;");
    check("recover: rc=OK", rc === RC_OK, rc);
    check("recover: oom_hit cleared", oom_hit() === 0);
    check("recover: requested cleared", oom_requested() === 0);
}

// ── A second independent OOM still reports cleanly ───────────────────
{
    const rc = arena_run_module("oom2.js",
        "let s=''; for (let i=0;i<200000;i++) s += 'xxxxxxxxxx'; globalThis._=s.length;");
    check("oom2: rc=OOM", rc === RC_OOM, rc);
    check("oom2: oom_hit=1", oom_hit() === 1);
}

console.log(`\n${passed}/${total} passed`);
if (passed !== total) process.exit(1);
