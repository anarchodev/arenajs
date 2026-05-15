# Replay Cursor API (sketch)

Design notes for the host-side API the browser scrubber uses to pull
trace events out of a deterministic replay. The engine surface is
already in place — `arena_set_trace_mode`, `host_trace`, scan/drill
modes, stop sentinel, the five replay tape channels (kv, Date.now,
Math.random, crypto.\*, module loader), and multi-module loading
through `Module.tapes.module` + `Module.module_sources`. This doc is
purely host-side policy on top.

## Model

Two tiers, with very different cost profiles:

- **Scan index** — one cheap pass per replay, cached. Linear list of
  scan events (`FUNC_ENTER` / `FUNC_EXIT` / `THROW`) with resolved
  name/file atoms and depth. Built once per replay; reused by every
  cursor that targets the same replay.

- **Drill cursor** — opaque position into the drill-mode event stream.
  Opens at a scan ordinal or a (file, line) anchor; client pulls
  pages of drill events (`LINE` plus the scan kinds, which still fire
  in drill mode) until end-of-window or end-of-replay.

V1 is **stateless across pages**: every page replays from the start of
the entry module, fast-forwards in scan mode to the anchor, flips to
drill, and collects N events. Tapes make this deterministic and
bounded; if profiling shows tight scrubbing dominated by re-replay,
layer a drill-chunk cache without changing the API.

## Execution model

The cursor module owns one long-lived arena per worker:

- `arena_init` is the expensive call — it builds the base runtime,
  installs replay bindings + module loader pre-freeze, then freezes.
  Done once at worker startup.
- Each page is a single `arena_run_module(entry_name, entry_src)` call.
  `JS_ResetRequestArena` runs at the top of each invocation and the
  trace name table is reset per run, so replays are clean against each
  other without tearing down the arena.
- `Module.tapes` and `Module.module_sources` are reinstalled on the JS
  side before each call (cursor against replay A, next page against
  replay B, same arena).
- The host_trace JS callback is the only place per-page state lives
  (event counter, atom→string map, scan ordinal tracker, drill page
  buffer). Resets between calls.

Pool size = number of concurrent cursor fetches you want in flight.
Single-arena-per-worker is fine for v1; the UI is the only client.

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
      afterScan?: ScanOrdinal;   // disambiguates repeat hits
      occurrence?: number };     // alt: nth hit overall, default 0

interface Cursor {
  replay: Replay;                // held by reference, not re-serialized
  anchor: Anchor;
  drillEventsAfterAnchor: number;
}

interface Page {
  events: DrillEvent[];          // sized for fat-fetch (see Cost shape)
  next: Cursor | null;           // null = end-of-replay
}

// API
scanIndex(replay: Replay): Promise<ScanRecord[]>;
openCursor(replay: Replay, anchor: Anchor): Cursor;
drillNext(cursor: Cursor, limit: number): Promise<Page>;
```

Cursors are plain JS objects held by the caller — no serialization or
signing, since the cursor module runs in-process with the UI. If a
future deployment puts the WASM in a separate worker or server, swap
`Cursor` for an opaque token without changing the verbs.

A content hash of `(entry.src, tapes bytes)` keys the scan-index cache;
that's an implementation detail of `scanIndex`, not part of the surface.

## Resolution

For `drillNext(cursor, n)`:

1. Look up scan index for the replay (build + cache on miss).
2. Install `cursor.replay.tapes` + `module_sources` on the Module
   object. Set trace mode to **scan**.
3. Install a `host_trace` handler that:
   - Maintains a NAME atom→string map (every `NAME` event interns).
   - Counts scan events to track the current scan ordinal.
   - When the scan ordinal reaches the anchor's bracketing scan event
     (from the cached index), flips trace mode to **drill** by calling
     `arena_set_trace_mode(2)`.
   - For `line` anchors: continues discarding drill events until the
     first `LINE` matching `(file, line)` past `afterScan`/`occurrence`
     — that is event 0 past the anchor.
   - Skips `cursor.drillEventsAfterAnchor` drill events.
   - Pushes the next up-to-`n` drill events (with atoms resolved + the
     current `scanOrdinal` stamped on each) into the page buffer.
   - Returns 1 to stop cleanly once the buffer is full.
4. Call `arena_run_module(entry.name, entry.src)`.
5. Return `{ events, next: cursor with offset += events.length }`,
   unless the run completed without filling the buffer, in which case
   `next: null`.

The scan→drill flip from inside `host_trace` is the trick that keeps
the fast-forward cheap: drill mode is only paid for the slice the
client actually wants.

## Stop conditions for a page

A page ends when any of these hit, whichever first:

- `n` drill events collected.
- Replay ends.
- (optional, opt-in) Next `FUNC_EXIT` at depth ≤ anchor depth —
  "drill within this frame only." Useful for "step over."

## Cost shape

- Scan-index build: one full replay, scan mode. O(scan events). Cached
  per replay identity.
- Each `drillNext` page: one full replay, scan mode up to anchor, drill
  mode from anchor to anchor+offset+n. Dominated by program runtime;
  tapes are RAM-resident so there's no I/O.

The dominant concern is **O(offset) per page**: page 100 redoes all
the drill work from pages 0..99 because each page replays from the
start. Implications for the default page shape:

1. **Fat pages, client-side scrubbing.** Default `limit` should be
   large (10k events or per-window) so the UI fetches once per scan
   window and scrubs locally. Small-`limit` paging is the wrong
   default for this cost shape.
2. LRU cache of `(replayId, scanOrdinal) → drill chunk` if profile
   shows repeated re-fetches dominating.
3. (Only if neither is enough) reintroduce a pinned-VM stepping session
   as a *separate* API; do not retrofit cursors to be stateful.

## Open questions

- **Line occurrence disambiguation.** `afterScan` is the precise knob,
  but UIs probably want `occurrence: nth time this line is hit`. Need
  to decide which is primary; `afterScan` composes better with the
  scan index already in hand.
- **Stop-and-inspect interplay.** `host_trace` returning 2 ships a
  full stack snapshot via `_arena_host_state`. For paged cursors, does
  each page optionally include the inspect blob for its last event, or
  is inspect a separate `inspectAt(cursor)` call that replays to that
  exact position? Probably the latter — keeps drill pages pure events.
- **Cross-window cursors.** Does a single cursor naturally walk past
  its anchor's next scan event, or does the client open a new cursor
  per scan window? Former is simpler for "play forward"; latter is
  simpler to cache. Lean toward former.
