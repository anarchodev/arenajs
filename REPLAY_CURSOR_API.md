# Replay Cursor API (sketch)

Design notes for the host-side API the browser scrubber uses to pull
trace events out of a deterministic replay. The engine surface is
already in place — `arena_set_trace_mode`, `host_trace`, scan/drill
modes, stop sentinel, the five replay tape channels (kv, Date.now,
Math.random, crypto.\*, module loader), and multi-module loading
through `Module.tapes.module` + `Module.module_sources`. This doc is
purely host-side policy on top, plus one small engine addition
(`arena_trace_snapshot_here`) that unlocks live variable inspection.

## Architecture

Two layers, both backed by the same long-lived arena:

- **Scan index** — one cheap pass per replay, cached. Linear list of
  scan events (`FUNC_ENTER` / `FUNC_EXIT` / `THROW`) with resolved
  name/file atoms and depth. Always available, cheap to build.
- **Materialisation** — one drill pass per replay, cached. Captures
  the full events stream + sparse stack snapshots + sparse variable
  snapshots + reverse indices into a single `Materialised` object.
  Everything the scrubber needs to render any position is then in
  RAM and O(1) addressable.

The cursor verbs (`drillNext` / `inspectAt`) are thin slices over the
`Materialised` object. There is **no per-page replay**: materialise
once on first access, slice many times.

For replays too large to materialise under a byte budget, the system
fails loud (returns an error / disables the drill scrubber in the UI)
rather than silently degrading to slow per-page replay. Lifting that
ceiling is the job of state checkpoints (future direction below) —
not of a stateless-cursor fallback.

## Execution model

The cursor module owns one long-lived arena per worker:

- `arena_init` is the expensive call — it builds the base runtime,
  installs replay bindings + module loader pre-freeze, then freezes.
  Done once at worker startup.
- Each operation against a replay runs `arena_run_module(entry_name,
  entry_src)` exactly once: the materialisation pass. `JS_ResetRequestArena`
  runs at the top of that invocation and the trace name table is reset
  per run, so replays are clean against each other without tearing
  down the arena.
- `Module.tapes` and `Module.module_sources` are installed on the JS
  side before the run; the host_trace JS callback consumes events into
  the per-replay `Materialised` object.
- Subsequent `drillNext` / `inspectAt` / scrub queries are pure
  in-memory operations against the cached `Materialised`.

`inspectAt` is the one exception: it re-runs `arena_run_module` to
deliver exact values at a specific event ordinal (with optional
cluster window). That cost is paid per user gesture (click /
arrow-key), not per scrubber frame.

## Surface

```ts
type Atom = number;              // QJS atom id, resolved via NAME events
type ScanOrdinal = number;       // index into scan index

interface Replay {
  entry: { name: string; src: string };
  tapes: RtapTapes;              // shape from rtap.mjs buildTapesFromBlobs
}

interface ScanRecord {
  ordinal: ScanOrdinal;
  kind: "FUNC_ENTER" | "FUNC_EXIT" | "THROW";
  name?: string;                 // FUNC_ENTER
  file: string;
  line: number;
  depth: number;                 // running enter/exit balance, host-computed
}

interface DrillEvent {
  kind: "FUNC_ENTER" | "FUNC_EXIT" | "LINE" | "THROW";
  file: string;
  line: number;
  name?: string;                 // FUNC_ENTER
  message?: string;              // THROW
  scanOrdinal: ScanOrdinal;      // most-recent scan event at this point
}

type Anchor =
  | { kind: "scan"; ordinal: ScanOrdinal }
  | { kind: "line"; file: string; line: number;
      afterScan?: ScanOrdinal;
      occurrence?: number };

interface Cursor {
  replay: Replay;
  anchor: Anchor;
  drillEventsAfterAnchor: number;
}

interface Page {
  events: DrillEvent[];
  next: Cursor | null;           // null = end-of-replay
}

interface StackFrame {
  name: string; file: string; line: number; depth: number;
}

interface VarBinding {
  name: string; value: string; truncated?: boolean;
}

interface VarSnapshot {
  eventOrdinal: number;
  frames: Array<{
    name: string; file: string; line: number;
    args: VarBinding[]; locals: VarBinding[]; closure: VarBinding[];
  }>;
}

interface Materialised {
  events: DrillEvent[];
  matchingExit: Int32Array;            // ENTER idx ↔ EXIT idx
  stackSnapshots: StackFrame[][];      // sparse, every K events (default 64)
  lineIndex: Map<string, Int32Array>;  // "file:line" → event indices
  varSnapshots?: VarSnapshot[];        // present iff snapshotStep was set
  // Caches populated lazily by inspectAt:
  inspectCache: Map<number, VarSnapshot>;
}

interface MaterialiseOptions {
  snapshotStep?: number;     // sample cadence for varSnapshots (0 = none)
  valueMaxLen?: number;      // stringification cap per value (default 256)
  byteBudget?: number;       // abort if materialised heap exceeds this
}

// API
scanIndex(replay: Replay): Promise<ScanRecord[]>;
materialise(replay: Replay, opts?: MaterialiseOptions): Promise<Materialised>;
openCursor(replay: Replay, anchor: Anchor): Cursor;
drillNext(cursor: Cursor, limit: number): Promise<Page>;
inspectAt(materialised: Materialised, eventOrdinal: number,
          opts?: { cluster?: number }): Promise<VarSnapshot[]>;
```

`drillNext` internally calls `materialise(cursor.replay)` (memoised
per replay) and slices from `materialised.events`. Callers get the
existing paged interface for compatibility; the cursor module handles
the materialise-then-slice pattern internally.

Cursors are plain JS objects held by the caller — no serialization or
signing, since the cursor module runs in-process with the UI.

A content hash of `(entry.src, tapes bytes)` keys both the scan-index
and materialise caches; that's an implementation detail, not part of
the surface.

## Resolution

For `materialise(replay, opts)`:

1. Look up scan index for the replay (build + cache on miss).
2. Install `replay.tapes` + `module_sources` on the Module object.
   Set trace mode to **drill**.
3. Install a `host_trace` handler that:
   - Maintains a NAME atom→string map (every `NAME` event interns).
   - Maintains a running call-frame stack: push on `FUNC_ENTER`, pop
     on `FUNC_EXIT`; record the matching-enter index for each exit
     into `matchingExit` (Int32Array).
   - Pushes each decoded event onto `events`. Stamps `scanOrdinal`.
   - Every K events (default 64), snapshots the running call-frame
     stack into `stackSnapshots`.
   - If `opts.snapshotStep` is set, every Nth event calls
     `arena_trace_snapshot_here()` and pushes the returned VarSnapshot
     onto `varSnapshots`.
   - Tracks `(file, line) → indices` for `lineIndex`.
   - If `events`-array byte estimate crosses `opts.byteBudget`,
     returns 1 to halt cleanly and the materialise call throws
     `BudgetExceeded`.
4. Call `arena_run_module(entry.name, entry.src)`.
5. Return the populated `Materialised`. Cache it.

`drillNext(cursor, n)` is a pure slice: ensures materialised, resolves
the anchor to an event-array index using the scan index + (file, line)
match, returns `events[anchorIdx + cursor.drillEventsAfterAnchor ..
+n]` as a `Page`. No engine involvement.

`inspectAt(materialised, K, { cluster })` re-runs the replay with
`snapshotStep = 1` from `K - cluster` to `K + cluster`, captures
`2*cluster + 1` snapshots via `arena_trace_snapshot_here()`, populates
`materialised.inspectCache` for those ordinals, returns the array.

## Byte budget and graceful degradation

`MaterialiseOptions.byteBudget` (default ~300 MB) is the ceiling at
which materialise gives up. When `materialise` throws `BudgetExceeded`:

- The UI surfaces the scan-index timeline as normal (scan index has no
  byte concern — scan events are sparse).
- The drill scrubber renders as disabled with an explanatory banner.
- No silent degradation to per-page replay. Per-page replay was the
  v1 sketch; benchmarks showed it's catastrophically slow for the
  sizes that exceed the materialise budget anyway, so its only honest
  use case (covering the gap above the budget) doesn't actually
  deliver usable UX. State checkpoints (future direction) are the
  proper answer.

The byte ceiling lines up with roughly 30 ms of pure-execution-budget
replays (uncompressed events array at ~104 B/event in V8). Columnar
representation (future direction) lifts this to ~150 ms without
checkpoints.

## Two access modes

The materialised data ends up serving two complementary access
patterns. Both share the same `Materialised` object — they're just
different ways to query it.

| mode  | when           | cost   | precision        | data source                                 |
|-------|----------------|--------|------------------|---------------------------------------------|
| scrub | drag, animate  | O(1)   | sample resolution| `events`, `stackSnapshots`, `varSnapshots`  |
| step  | arrow, click   | ~ms    | exact            | `inspectAt` (replay-to-K + full inspect)    |

**Scrub mode** is for continuous navigation where the user is sweeping
through positions faster than any per-event replay could keep up.
Always-visible state (event ordinal, current line, current call stack)
is exact at every K because it's derived from the dense events array
plus sparse stack snapshots. Variable values are exact at sample
points (every `snapshotStep` events) and stale-but-displayed in between
— the UI shows them with the nearest sample's values until the next
sample is reached, which at scrubber resolution looks continuous.

**Step mode** is for discrete navigation where the user has landed on
a specific position and wants the truth. The UI triggers
`inspectAt(materialised, K)`, which does the one-shot replay-to-K via
the snapshot-while-running engine path, caching results on the
materialised object. Latency is acceptable because each step is a
deliberate user gesture, not a frame in an animation.

The UI orchestrates between them: pointer-down on the scrubber → scrub
mode; pointer-up (or arrow-key, or click on an event) → step mode and
issue an inspectAt for the landed position. The transition is
imperceptible: scrub mode was already showing approximately-right
values, step mode replaces them with exact ones a few ms later.

**Step-back-from-sample.** If the user lands at event K and arrows
back to K-1, K-1 isn't a sample point so an inspectAt is needed. The
trick: `inspectAt` accepts a **cluster width** so the same replay
captures snapshots for a window around K, not just K itself. Returns
1 snapshot for `cluster: 0` (default), `2*cluster+1` snapshots for
`cluster: M` (events `K-M..K+M`). Under the hood it sets
`snapshotStep = 1` and runs from `K-M` to `K+M` capturing every event;
the replay-to-K is the expensive part, and capturing additional
snapshots once already there is essentially free.

Results are indexed by ordinal and cached on the materialised object,
so subsequent fine-steps anywhere inside an already-fetched cluster
are O(1). Holding down arrow-key stays instant after the first ~ms
hit, and the UI can background-prefetch the next adjacent cluster as
the user approaches the current cluster's edge.

## Engine touch point (the only C addition)

```c
/* qjs-arena-trace.h — new export */

/* Walk the live stack and ship the same inspection payload that
 * host_trace=2 emits, via _arena_host_state. Unlike host_trace=2,
 * does NOT raise the stop sentinel — execution continues.
 *
 * Must be called synchronously from inside a host_trace callback.
 * Returns 0 on success, -1 if called outside a trace event. */
int arena_trace_snapshot_here(void);
```

Implementation reuses the existing stack-walk + `_arena_host_state`
emit path from the host_trace=2 handler in `qjs-arena-trace.c`; the
only difference is omitting the final `JS_Throw(... ARENA_TRACE_STOP_MSG)`.
A few tens of lines.

This is a thin engine addition that enables a host capability
(per-event opt-in to inspection) rather than embedding host policy
(when to inspect). Cadence selection stays in JS.

## Cadence policy

The host_trace callback can't afford a per-event JS predicate at 3M
events/sec, so cadence is expressed as a step. UI sets
`snapshotStep = Math.floor(estimatedEventCount / scrubberPixels)` for
pixel-resolution scrubbing. Event-count estimation is loose; two
reasonable options:

1. Always run scan-index first; estimate `events ≈ scanRecords × K`
   for some K (10–20 fits typical workloads).
2. Run materialise once with `snapshotStep` zero, then a second pass
   with the right step. Pays the drill cost twice; only worth it if
   the first option's estimate is too far off.

## Value stringification

Per-binding `value` strings are produced engine-side (the existing
`host_trace=2` path already does it) and capped at `valueMaxLen`
chars. When a value is truncated, `truncated: true` flags it so the
UI renders a `(more)` affordance. Clicking `(more)` triggers
`inspectAt(materialised, eventOrdinal)` with no truncation cap —
delivers the full value(s) for that snapshot's position. The expanded
result is cached on the materialised object keyed by
`(eventOrdinal, frameIdx, name)` so re-opening the same expanded
value is free.

## Cost shape

For an 800 px scrubber over a 10 ms request:

```
~250 ms  drill all events
+ ~80 ms (800 stack walks × ~100 µs for shallow stacks)
≈ ~330 ms total

events array      65 MB
stackSnapshots     6 MB
varSnapshots    1–6 MB  (800 × 1–8 KB depending on stack depth + locals)
lineIndex       ~1 MB
matchingExit    0.5 MB
total          ~75 MB
```

After this single materialise pass, every scrubber frame is array
indexing + at most 64 delta applications. 60 FPS bidirectional
scrubbing trivially in reach. Click-for-precision (`inspectAt`) pays
~ms per gesture and caches.

Per pure-execution budget:

| budget | drill events | materialised heap | drill-all time |
|--------|--------------|-------------------|----------------|
| 1 ms   | ~65k         | ~7 MB             | ~25 ms         |
| 10 ms  | ~630k        | ~65 MB            | ~250 ms        |
| 30 ms  | ~1.9M        | ~200 MB           | ~750 ms        |
| 100 ms | ~6.3M        | ~650 MB           | ~2.5 s         |

The 30 ms / 200 MB row is roughly the default byte-budget ceiling.
Beyond that, materialise throws and the UI falls back to the
scan-only timeline until checkpoints lift the ceiling.

## Future direction: state checkpoints for long-running requests

At a 10 ms request budget, the materialise pass is fast and cheap.
For customers with multi-second request limits (5 s and up),
materialising the whole drill stream becomes prohibitive — a 5 s
request at ~3M drill events/sec would produce ~15M events / ~1.5 GB
events array.

The unlock is **mid-execution checkpoints**: during the materialise
pass, periodically save enough VM state that later cursor operations
can resume from the nearest checkpoint instead of from the start of
the request. arenajs's design makes this dramatically simpler than it
would be in stock QuickJS:

- **Base is immutable post-freeze.** Bytecode, prototypes, builtins,
  the default context — none of it changes during a request. Zero
  checkpoint cost.
- **Request arena is bump-allocated, contiguous, no GC.** All mutable
  JS heap lives in one arena with a known top pointer. Snapshot is
  `memcpy(arena[0..top])`. Restore copies back to the same virtual
  address; pointers inside the arena resolve correctly because the
  base mmap is at a fixed address. CoW thermometer (see
  `ARENA_PLAN.md`) gives page-granularity deltas if total size hurts.

The hard part is the **C call stack**. QuickJS's dispatch loop
recurses on the native stack — each JS call is a `JS_CallInternal` C
frame. Saving and restoring those by raw bytes is fragile. But
QuickJS-ng already has the right machinery: async functions and
generators suspend a frame mid-execution, save its locals / args /
closure / PC into a heap structure, unwind the C stack, and later
resume via a fresh `JS_CallInternal` that picks up at the saved PC.
The checkpoint flow reuses that:

- **Checkpoint** = `memcpy(arena)` + walk the live `JSStackFrame`
  chain and "suspend" each one (same path as generator suspend),
  recording chain order, plus tape cursors and microtask queue head
  (both already in arena).
- **Restore** = `memcpy(arena)` back + replay the saved frame chain
  as nested generator-style resumes from the bottom up. The final
  resume picks up at the innermost frame's PC and execution continues
  normally.

Storage shape (back-of-envelope for a 5 s request):

| checkpoint interval | count | per-snapshot | total (full) | total (CoW) |
|---------------------|-------|--------------|--------------|-------------|
| 500 ms              | ~10   | 10–100 MB    | ~100 MB–1 GB | tens of MB  |
| 200 ms              | ~25   | 10–100 MB    | ~250 MB–2.5 GB | similar    |

Server- or worker-side this is comfortable; in-tab it wants CoW
deltas. With 500 ms checkpoints, worst-case "replay from nearest
checkpoint" is ~500 ms of execution ≈ ~50 ms in drill mode — every
cursor operation stays responsive regardless of K's position in the
replay, and `Materialised` becomes a lazy view: only the slice the UI
currently shows is in RAM.

API impact on the cursor surface is minimal: `Materialised` becomes
lazily-populated (window of currently-visible events held, rest
on-demand from nearest checkpoint), and an internal
`checkpoints: Checkpoint[]` field on `Materialised` drives the
routing. Caller-visible signatures don't change.

**Effort estimate.** Engine work is the bulk — adapting the generator
suspend/resume machinery to handle arbitrary frame chains rather than
just async-function bodies. ~1–2 weeks focused. Unlocks when customer
pressure (multi-second limits) makes materialise-the-whole-replay the
bottleneck. Worth flagging now so the cursor API stays
forward-compatible.

## Open questions

- **Line occurrence disambiguation.** `afterScan` is the precise knob,
  but UIs probably want `occurrence: nth time this line is hit`. Need
  to decide which is primary; `afterScan` composes better with the
  scan index already in hand.
- **Default byte budget.** 300 MB is the headline; the right number
  depends on browser tab heap pressure under realistic concurrent UI
  state. Worth empirical tuning once the UI is running.
- **Cross-window cursors.** Does a single cursor naturally walk past
  its anchor's next scan event, or does the client open a new cursor
  per scan window? Former is simpler for "play forward"; latter is
  simpler to cache. Lean toward former.
- **Columnar event representation.** TypedArrays of kind / file-id /
  line / depth / scanOrdinal would drop per-event cost from ~104 B
  to ~20 B, lifting the materialisation ceiling from ~30 ms requests
  to ~150 ms. Worth doing only when the 30 ms ceiling proves to be a
  problem in practice.
