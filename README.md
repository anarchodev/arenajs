# arenajs

A fork of [quickjs-ng](https://github.com/quickjs-ng/quickjs) tuned for
**one-shot, per-request JS execution on a server**. The runtime is
initialized once into a frozen "base" snapshot; each request runs in a
"request" arena that resets in **a single bump-cursor write
(~9 ns)**. No GC, no `free` calls, no memcpy of the snapshot.

This README is the entry point for embedders. If you're here from the
**rove** project to swap out the existing memcpy-restore vendor, jump
to [Migrating from a memcpy-restore vendor](#migrating-from-a-memcpy-restore-vendor).
For the browser-side time-travel replay UI (scrubbing / stepping a
recorded request), see
[Replay scrubbing & stepping](#replay-scrubbing--stepping-wasm).

## What's different vs upstream quickjs-ng

| | upstream | arenajs |
|---|---|---|
| Allocator | system malloc + GC | dual bump arena |
| Per-request reset cost | N/A (long-lived runtimes) | **9 ns** (one store) |
| Free / GC | refcount + cycle collector | both no-op; arena is the GC |
| Determinism | wall-clock random/time | seeded; `Math.random()`, `performance.timeOrigin`, `Date.now()` are deterministic by default |
| Snapshot model | external (memcpy + bitmap relocation) | **internal** (cursor reset over a frozen base) |
| Spec coverage | full test262 | full test262, **verified base-clean** (see below) |

The headline trade-off: **the snapshot's base bytes are inviolate**.
No JS code (not even pathological) is allowed to mutate snapshot
memory. Anywhere upstream would write through into a base JSObject /
JSShape / atom, arenajs either pre-forces the work at freeze, gates
the write, or shadows the target into the request arena (path-copy
style). The verification this works is in
[`arena-test262.c`](arena-test262.c): it runs the entire test262
corpus under a page-protected base + SIGSEGV thermometer.

**Current state:** `46,020 / 46,020` test262 tests across all five
trees (built-ins, language, annexB, staging, intl402) leave zero
base bytes touched and do not crash. See `make test-arena`.

## API at a glance

The whole new API surface is in [`qjs-arena.h`](qjs-arena.h). The
core lifecycle is three calls:

```c
#include "quickjs.h"
#include "qjs-arena.h"

/* 1. Create the runtime + context, do all init.
 *    base_size and request_size are fixed mmap'd buffers; pass 0
 *    for the 16 MiB default. Allocations beyond capacity return NULL
 *    and propagate as JS OOM. */
JSRuntime *rt = JS_NewRuntimeArena(/*base*/ 64 * 1024 * 1024,
                                   /*request*/ 16 * 1024 * 1024);
JSContext *ctx = JS_NewContext(rt);

/* ... eval prelude scripts, register native functions, build any
 * snapshot-time globals — all of this lands in the BASE arena ... */

/* 2. Freeze: flips allocation mode to the request arena, pre-forces
 *    every lazy initializer, pre-marks every base prototype, then
 *    relocates the per-request mutable runtime state. After this
 *    point, NO further base bytes are written by JS execution. */
JS_FreezeRuntime(rt);

/* 3. Per-request loop. */
for (;;) {
    JSValue v = JS_Eval(ctx, request_src, len, "<req>", JS_EVAL_TYPE_GLOBAL);
    /* ... use v, marshal a response ... */
    JS_FreeValue(ctx, v);

    /* Reset: one store rewinds the request-arena cursor and re-inits
     * the per-request state struct. ~9 ns. The base snapshot
     * (prototypes, atoms, bytecode, ICs) is unchanged and shared. */
    JS_ResetRequestArena(rt);
}
```

There is no `JS_FreeRuntime` / `JS_FreeContext` step in arena mode.
Teardown is `js_dual_arena_free(JS_GetDualArena(rt))`, which `munmap`s
the buffers. The runtime is intentionally non-disposable as a
running entity — it's an immortal piece of process state.

### Determinism helpers

Used at request boundaries to seed per-request behavior without
mutating base:

```c
/* Per-request RNG state. State is reset to 0 (Math.random() == 0)
 * on every JS_ResetRequestArena. Call before evaluating the request
 * if you want non-zero output. */
JS_SetRandomSeed(ctx, request_seed);

/* Per-request time origin. performance.timeOrigin is a getter
 * reading whatever you set here; performance.now() is relative. */
JS_SetTimeOrigin(ctx, wall_clock_ms);
```

Both are stored in `JSRequestState` (request arena) so the writes
don't dirty base.

### Verification (the thermometer)

`qjs-arena.h` exposes an mprotect-based "thermometer" used by the
test harnesses to assert zero base writes per request. Embedders
generally don't need it in production, but it's the right tool when
debugging a regression:

```c
JS_FreezeRuntime(rt);
js_arena_thermometer_enable();
for (each request) {
    js_arena_thermometer_reset();
    /* ... eval ... */
    if (js_arena_thermometer_pages() != 0) abort();  /* base was written */
    JS_ResetRequestArena(rt);
}
```

There's also `js_arena_thermometer_changed_bytes()` for byte-level
attribution and `js_arena_thermometer_trace_range(lo, hi)` to print
a backtrace at every fault inside a chosen offset window — used to
identify which call paths still mutate base when adding a new feature.

## Constraints

These follow from the model and aren't going to change:

- **A runtime stays on its creating thread.** Per-thread state is
  stored in `__thread` slots (the registered arena-range list, the
  per-runtime `is_arena` flag is fine cross-thread but the range
  list isn't). A thread that didn't create the runtime will see an
  empty range list and treat its base objects as request-arena,
  eventually corrupting refcounts. QuickJS is already single-
  threaded per-runtime, so this matches intent — but the runtime
  cannot migrate between threads.
- **Multiple arena runtimes can share one thread, and arena and
  vanilla (non-arena) runtimes can coexist.** Each
  `JS_NewRuntimeArena` registers its base range in the per-thread
  list (cap: 16 ranges); `js_arena_ptr_is_base` walks the list, so
  vanilla heap pointers (in no range) get the normal codepath while
  arena pointers (in some range) get the arena codepath. Per-runtime
  gating uses the new `rt->is_arena` flag set at `JS_FreezeRuntime`.
  See `arena-coexist.c` for a worked example.
- **The thermometer also supports multiple ranges concurrently.**
  The `SIGSEGV` handler is process-singleton (sigaction is process-
  wide), but it dispatches by `si_addr` to a list of per-range
  `(bitmap, baseline, counters)` entries (cap: 8). Two threads
  enabling the thermometer on the *same* range would race on its
  counters; that's an embedder error since arena ownership is
  per-thread. The no-arg API (`js_arena_thermometer_pages()` etc.)
  operates on the most recently enabled range; for multi-range
  testing call the `_range(lo, hi)` variants.
- **Single context per runtime.** A few JSRequestState fields
  (`error_back_trace_req`, `random_state_req`) are per-runtime, not
  per-context. Multi-context support would require those to become
  per-context like `shadow_map`.
- **No `JS_FreeRuntime` / `JS_FreeContext` after freeze.** GC is
  suppressed; refcounts on base objects are no-ops. Teardown is
  `js_dual_arena_free`.
- **Fixed buffers.** `base_size` and `request_size` are mmap'd at
  creation and never grow. Sizing them is up to the embedder;
  smoke/stress harnesses use 16 MiB / 16 MiB and 64 MiB / 16 MiB.
- **Weak references on base targets are no-ops.** WeakMap/WeakSet/
  WeakRef/FinalizationRegistry skip the cleanup-notification chain
  for base targets because base targets are immortal — the
  collections still work as strong references during the request.

## Building

CMake, same as upstream:

```sh
cmake -B build
cmake --build build -j
```

Targets produced:
- `build/qjs`, `build/qjsc`, `build/api-test`, `build/run-test262`
  (unchanged from upstream)
- `build/arena-smoke` — basic two-request lifecycle smoke test
- `build/arena-stress` — 38 JS workloads (Array methods, Object,
  String, JSON, Map/Set, closures, classes, try/catch, generators,
  destructuring, RegExp, Date, Symbol, Proxy, Promise, defineProperty)
- `build/arena-bench` — per-request reset speed benchmark
- `build/arena-test262` — the test262 walker (requires the test262
  submodule, see below)
- `build/arena-coexist` — vanilla + arena runtime sharing one thread,
  100 interleaved request iterations with the thermometer asserting
  zero base writes per arena request
- `build/arena-interrupt` — verifies `JS_SetInterruptHandler` fires
  in arena mode (per-request counter on `JSRequestState` instead of
  the base ctx) and that aborting an infinite-loop request leaves
  zero base bytes touched

### Running the regression sweep

```sh
git submodule update --init --depth=1 test262   # one-time
make test-arena                                  # builds + runs everything
```

`make test-arena` runs all four arena harnesses in order. The
arena-test262 walker runs all five test262 trees; any non-zero
"base dirtied" count is a regression.

### Bench numbers (Release, single thread)

```
A: globalThis.blah set                  3682 ns/iter
B: override base GREETING               3437 ns/iter
C: delete base GREETING                 3445 ns/iter
D: 100x push + reduce                  18541 ns/iter
reset alone (floor)                        9 ns/iter
```

Most of the per-request cost is parse + compile + eval of the source.
The reset itself is the floor (9 ns). Pre-compiled bytecode gets you
closer to the floor.

## Replay scrubbing & stepping (WASM)

The same determinism that makes per-request reset cheap also makes a
**recorded request replayable, bit-for-bit, in the browser** — which
is what powers a time-travel scrubber/stepper UI (the rove replay
view). You record a request's non-deterministic inputs once
(`kv` get/set/delete/prefix, `Date.now`, `Math.random`, `crypto.*`,
the module loader) onto *tapes*; replaying the same entry script
against those tapes re-executes the exact same program, so you can
trace it as deeply as you like after the fact without the original
environment.

Design rationale and the full data model are in
[`REPLAY_CURSOR_API.md`](REPLAY_CURSOR_API.md); build instructions for
the WASM target are in [`tests/wasm/README.md`](tests/wasm/README.md).
The host module described here (`tests/wasm/cursor.mjs`) is staged in
this repo pending a lift into rove's tree — it is **tooling, not part
of the embedder contract** (see [`CHANGELOG.md`](CHANGELOG.md)).

### The pieces

- **WASM build target `qjs_arena_wasm`.** Same arena runtime compiled
  under Emscripten with `ARENA_TRACE_ENABLED=1` and
  `-sALLOW_MEMORY_GROWTH=1`. Exposes `arena_init`, `arena_run_module`,
  `arena_set_trace_mode`, `arena_snapshot_here`, the `arena_oom_*`
  query functions, and `arena_destroy`. The native worker build has
  the trace machinery compiled out (zero overhead) — scrubbing is a
  WASM-only surface.
- **Trace modes** (`arena_set_trace_mode`): `OFF` (0), `SCAN` (1) —
  `FUNC_ENTER` / `FUNC_EXIT` / `THROW` only, cheap — and `DRILL` (2),
  which adds a `LINE` event on every source-line transition. A
  `host_trace` JS callback receives binary event payloads; returning
  truthy stops execution cleanly via a sentinel.
- **`arena_snapshot_here()`** walks the live stack and ships a JSON
  snapshot (function/file/line + args/locals/closure vars per frame)
  via a `host_state` callback **without** stopping execution — so one
  replay pass can capture many variable snapshots.
- **`CursorEngine`** (`tests/wasm/cursor.mjs`) wraps an
  `arena_init`'d module and turns all of the above into a navigable
  timeline.

### Using the cursor module

```js
import getArenaJs from "./build-wasm/qjs_arena_wasm.js";
import { CursorEngine } from "./tests/wasm/cursor.mjs";
// import { buildTapesFromBlobs } from "./tests/wasm/rtap.mjs";  // RTAP → tapes

const Module = await getArenaJs();
// Size the request arena for the replay's churn (see "Sizing" below).
if (Module.cwrap("arena_init", "number", ["number","number"])(8192, 16384) !== 0)
    throw new Error("arena_init failed");
const eng = new CursorEngine(Module);

// A replay = the entry script + the recorded tapes it consumes.
const replay = {
    entry: { name: "handler.js", src: recordedEntrySource },
    tapes: recordedTapes,            // from buildTapesFromBlobs(rtapBytes)
    module_sources: recordedModules, // path-keyed import sources
};

// 1. Cheap coarse timeline — call/throw structure, no LINE events.
const idx = await eng.scanIndex(replay);   // ScanRecord[]

// 2. One drill pass → everything the scrubber needs in RAM.
//    targetSnapshots = scrubber pixel width: the engine picks the
//    variable-snapshot cadence so you get ~that many samples.
const mat = await eng.materialise(replay, { targetSnapshots: 800 });
//   mat.events                 dense DrillEvent[] (indexable by ordinal)
//   mat.scanOrdinalToEventIdx   scan ordinal → events index
//   mat.stackSnapshots[/step]   live call stack every stackSnapshotStep
//   mat.matchingExit            ENTER idx ↔ EXIT idx (both directions)
//   mat.lineIndex               "file:line" → event indices
//   mat.varSnapshots[/step]     variable values at the chosen cadence
```

**Scrub mode** — continuous drag/animation, O(1) per frame, sample
resolution. Map the scrubber pixel to an event ordinal and index
straight into the materialised arrays; nothing re-runs:

```js
function frameAt(K) {                       // K = event ordinal
    const ev    = mat.events[K];
    const stack = mat.stackSnapshots[Math.floor(K / mat.stackSnapshotStep)];
    const vars  = mat.varSnapshots?.[Math.floor(K / mat.varSnapshotStep)];
    return { ev, stack, vars };             // current line + call stack + values
}
```

Always-visible state (event, current line, call stack) is exact at
every K; variable values are exact at sample points and
stale-but-shown between them, which reads as continuous at scrubber
resolution.

**Step mode** — discrete arrow-key / click, ~ms, *exact*. Re-runs the
replay to the landed position and snapshots a window around it; the
window is cached on `mat.inspectCache` so subsequent fine-steps inside
it are O(1):

```js
// Exact vars at K plus K-5..K+5 prefetched for instant arrow-keying.
const snaps = await eng.inspectAt(mat, K, { cluster: 5 });
const here  = snaps.find(s => s.eventOrdinal === K);
```

For "play forward from here" / windowed consumption rather than random
access, use the paged cursor:

```js
const cur  = eng.openCursor(replay, { kind: "scan", ordinal: someScanOrd });
let   page = await eng.drillNext(cur, 5000);   // { events, next }
while (page.next) page = await eng.drillNext(page.next, 5000);
```

`openCursor` also accepts a `{ kind: "line", file, line, afterScan }`
anchor to jump to a source position.

### Sizing & failure signal

Replay re-execution allocates into the request arena, which does not
reclaim mid-run. Size `arena_init`'s request arena for the replay's
*cumulative* allocation, not its peak — a churning handler needs more.
`materialise` with `targetSnapshots` is two passes (count, then
snapshot at the right cadence) and the snapshot path serializes
directly to bytes, so deep stacks no longer blow the arena; the bench
in `tests/wasm/cursor-ui-bench.mjs` covers tight-loop / call-heavy /
deep-recursion / mixed shapes.

If a replay exhausts the arena, the signal is unambiguous and
actionable rather than a mystery failure:

```js
const rc = Module.cwrap("arena_run_module","number",["string","string"])(name, src);
//  0  ok / clean stop
// -1  the JS threw (a bug in the recorded handler)
// -2  request arena exhausted — bump arena_init's request size
if (Module.cwrap("arena_oom_hit","number",[])()) {
    const used  = Module.cwrap("arena_oom_used","number",[])();
    const limit = Module.cwrap("arena_oom_limit","number",[])();
    // surface "replay needs a larger arena (used U / limit L)"
}
```

(`CursorEngine` drives `arena_run_module` for you; you only need the
raw return code / `arena_oom_*` if you embed the WASM module directly.)

## Migrating from a memcpy-restore vendor

If you currently vendor quickjs-ng with a custom snapshot/restore
layer (the rove pattern: bump arena + `memcpy` of a frozen image +
bitmap-driven pointer relocation + per-context volatile-slot reseed),
the swap is mostly *deletion*:

1. **Replace your vendored quickjs-ng files** with this repo's
   versions. The C sources you need are:
   ```
   quickjs.c quickjs.h
   qjs-arena.c qjs-arena.h     ← NEW, must add to your build
   libregexp.c libregexp.h libregexp-opcode.h
   libunicode.c libunicode.h libunicode-table.h
   dtoa.c dtoa.h
   cutils.h list.h
   quickjs-atom.h quickjs-c-atomics.h quickjs-opcode.h
   builtin-array-fromasync.h builtin-iterator-zip.h builtin-iterator-zip-keyed.h
   ```
   Add `qjs-arena.c` to your existing C-sources list. No other build
   flag changes — `-D_GNU_SOURCE` and the rest carry over verbatim.

2. **Drop your bump arena malloc-funcs.** `JS_NewRuntimeArena`
   installs `js_dual_arena_malloc_funcs` for you. The base/request
   split lives inside `JSDualArena` (one mmap'd buffer per side).

3. **Drop your snapshot create/restore code.** Replace:
   - `Snapshot.create(init_fn)` → `JS_NewRuntimeArena(...)` + run
     init eagerly + `JS_FreezeRuntime(rt)` once.
   - `Snapshot.restore(target_arena)` → `JS_ResetRequestArena(rt)`.
     No memcpy, no bitmap walk, no pointer relocation — the base
     buffer is shared in place across all requests.

4. **Drop the volatile-slot machinery.** No more two-pass diff to
   detect non-deterministic JSContext fields. Determinism is
   built-in: `Math.random()` and `performance.timeOrigin` start at
   0 every request; seed via `JS_SetRandomSeed` /
   `JS_SetTimeOrigin` if needed.

5. **Drop any "stable shape hash" or determinism patch you'd
   applied** to upstream quickjs-ng. Equivalent fixes are already
   in the fork (and verified by `make test-arena`).

6. **Drop `JS_FreeRuntime` / `JS_FreeContext` from your hot path.**
   Lifetime ends with `js_dual_arena_free` at process exit (or
   never, if the runtime is process-singleton).

After the swap, your per-request loop becomes:

```c
/* startup */
rt  = JS_NewRuntimeArena(BASE_SZ, REQ_SZ);
ctx = JS_NewContext(rt);
eval_prelude(ctx);
JS_FreezeRuntime(rt);

/* hot loop */
for each incoming request {
    JS_SetRandomSeed(ctx, random_u64());      // optional
    JS_SetTimeOrigin(ctx, wall_clock_ms());   // optional
    handle(ctx, request_body);                // calls JS_Eval, etc.
    JS_ResetRequestArena(rt);                 // 9 ns
}
```

A complete worked example is [`arena-smoke.c`](arena-smoke.c) — two
requests with a prelude, the thermometer wired up, and the
determinism helpers exercised.

## License

Same as upstream quickjs-ng: MIT. See [`LICENSE`](LICENSE).
