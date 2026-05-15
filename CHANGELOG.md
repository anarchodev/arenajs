# Changelog

All notable changes to the arenajs **embedder contract** are recorded
here. Format follows [Keep a Changelog](https://keepachangelog.com/);
versioning is [Semantic Versioning](https://semver.org/) applied to
the contract, not to the fork as a whole.

**Base:** quickjs-ng 0.14.0. arenajs versions independently; the
upstream base is recorded here as lineage metadata and is not coupled
to arenajs's release cadence.

## What semver covers here

The version applies to the surface an embedding product depends on:

- the exported `arena_*` reactor functions and their signatures
- the `arena_run` / `arena_run_module` return-code contract
- build flags required for an embedder build to keep working
- the replay/host wire formats (RTAP, `Module.tapes` /
  `Module.module_sources`, the trace-event binary encoding, the
  snapshot JSON shape)

It does **not** cover internal engine implementation, the arena
mechanics themselves, or the `tests/wasm/` host tooling (`cursor.mjs`
et al. are staged here pending a lift into rove's tree and are
versioned with their eventual home, not this contract).

Bump rules for this project:

- **MAJOR** — remove/rename an export, change a signature, change a
  return code's meaning incompatibly, break a wire format, or require
  a new build flag just to preserve existing behavior.
- **MINOR** — new export, new optional capability, a new return code
  that `rc !== 0` consumers degrade on gracefully, a new tape channel.
- **PATCH** — behavioral fix with no surface change.

Entries that change observable contract semantics — even when the
version bump is only MINOR — are flagged **⚠ Contract** with the
safe consumer pattern spelled out.

## [Unreleased]

_Nothing yet._

## [0.1.0] - 2026-05-15

First tracked version. Covers the browser-side replay cursor /
scrubber surface and the arena OOM signal.

### Added

- `arena_snapshot_here()` reactor export: walk the live stack and
  ship the inspection JSON via `_arena_host_state` **without** raising
  the stop sentinel (unlike `host_trace == 2`). Callable synchronously
  from a `host_trace` callback; returns 0 on success, -1 outside an
  active trace event. Enables variable snapshots during a single
  replay pass.
- `arena_oom_hit()`, `arena_oom_requested()`, `arena_oom_used()`,
  `arena_oom_limit()` reactor exports: query whether the request arena
  was exhausted this run and the numbers to act on it.
- Host-side replay cursor module (`tests/wasm/cursor.mjs`,
  pre-contract tooling): `scanIndex`, `materialise`, `openCursor`,
  `drillNext`, `inspectAt`. `materialise()` does one drill pass
  capturing the events array plus `stackSnapshots`, `matchingExit`,
  `lineIndex`, `scanOrdinalToEventIdx`; `inspectAt(mat, K, {cluster})`
  gives exact-position variable inspection with a cached window for
  instant fine-stepping. `materialise` takes `snapshotStep`
  (explicit) or `targetSnapshots` (auto cadence from the scrubber
  pixel width).
- Benches: `cursor-bench`, `cursor-mem`, `cursor-baseline`,
  `cursor-ui-bench` (UI access patterns across trace shapes), and
  smokes `snapshot-here-smoke`, `oom-smoke`.

### Changed

- **⚠ Contract — `arena_run` / `arena_run_module` return codes.**
  Previously any failure returned `-1`. Now:
  `0` success or clean host-requested stop, `-1` JS exception (user
  error), `-2` request arena exhausted (capacity — result is void).
  Safe consumer pattern is unchanged: `rc !== 0` still means
  "failed". **Consumers that exact-match `rc === -1` will now miss
  the OOM case** and must switch to `rc < 0` / explicit `-2` handling.
  Conservative policy: any refused request-mode allocation taints the
  whole run (`-2`) regardless of how execution ended, *except* an
  intentional clean stop.
- Replay cursor `drillNext` is now a pure slice over a one-time
  `materialise()` instead of a per-page replay. Public API unchanged;
  the stateless per-page cursor is retired. (Pre-contract tooling.)
- **Build:** the `qjs_arena_wasm` target now requires
  `-sALLOW_MEMORY_GROWTH=1` (added). The previous 16 MB INITIAL
  default was a hard cap with zero headroom; embedder builds that
  size the request arena generously need growth enabled.

### Fixed

- Dense-snapshot replays no longer exhaust the request arena.
  `emit_state` previously built a JSValue tree per snapshot (frame
  objects, var maps) into the bump arena, which never reclaims
  mid-run; deep stacks × dense cadence OOM'd. It now serializes the
  stack walk directly into a libc-malloc'd byte buffer — primitives
  format inline with zero JS allocation, only complex values cost a
  transient stringify. Deep recursion that needed 128 MB+ (and still
  OOM'd) now fits in 8 MB; materialise + snapshots is 2–3× faster.
  Snapshot JSON shape is unchanged.
- Arena exhaustion no longer surfaces as an unrecoverable
  `exception: (null)`. QJS-ng can't allocate the `Error` object under
  exhaustion so `current_exception` became bare `null`; the cause is
  now recorded at the allocator (the only place that knows for
  certain) and reported via the return code + `arena_oom_*`. The
  stderr line is now an actionable "request arena exhausted — needed
  N B, U / L B used" instead of "(null)".
- Trace stop sentinel is recognized even when its own `Error`
  construction OOM'd (`arena_trace_stop_armed()` records that stop was
  requested), so a host-requested stop under arena pressure is a
  clean `rc=0` rather than a spurious error.
- `arena_run` / `arena_run_module` exception diagnostics print the
  exception tag plus independently-probed name/message/stack instead
  of a single `JS_ToCString` that itself fails under pressure.
