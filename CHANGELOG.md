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

### Fixed

- **⚠ Contract** — post-freeze modifications of base (snapshot)
  objects were invisible to any lookup that reached the object through
  the prototype chain, rather than directly. `Map.prototype.set = f`
  read back correctly from `Map.prototype.set` but `(new Map()).set`
  still called the snapshot original; same for setters, `in`, `for-in`
  enumeration, `Object.keys` on the shadowed object,
  `Object.setPrototypeOf` on a base object, and `instanceof` after a
  proto mutation. Root cause: chain walks advanced with a bare
  `p = p->shape->proto` and never consulted the shadow overlay; only
  the walk's *starting* object was redirected. All chain walks
  (`JS_GetPropertyInternal`, `JS_SetPropertyInternal`, `JS_HasProperty`,
  `JS_GetPrototype`, `JS_OrdinaryIsInstanceOf`, the setPrototypeOf
  cycle check, the interpreter get_field/get_field2/get_length fast
  paths) now redirect each hop through `js_object_active`;
  `JS_GetOwnPropertyNamesInternal` gained the same entry redirect the
  other own-property readers already had. Read-only redirects: no new
  base writes (test262 walker stays 0-dirty), and no measurable cost
  on the arena benches (the redirect early-outs on non-base pointers).
  Consumer note: handlers that monkey-patch snapshot prototypes now
  actually take effect through instances — code that accidentally
  relied on the old half-applied behavior will see the override win.

- **⚠ Contract** — `Object.prototype.__proto__ = x` (and
  `Reflect/Object.setPrototypeOf` reached via the `__proto__` setter)
  failed to throw the required TypeError for the immutable-prototype
  exotic object post-freeze: the setter receives the shadow of
  Object.prototype as `this`, and the immutability identity check
  compared the shadow pointer against the base `class_proto`, silently
  missing. The wrongly-written prototype then lived invisibly in the
  shadow — and once chain walks honored shadows (fix above), it became
  a live prototype cycle that hung the first lookup to walk it.
  Identity checks in `JS_SetPrototypeInternal` now normalize through
  `js_object_base_identity()` (the inverse of the shadow redirect), so
  they hold regardless of whether a base or shadow pointer arrives.
  Full test262 sweep after both fixes: 46,020/46,020 base-clean, no
  hangs.

- Teardown of deep object graphs no longer risks C stack overflow.
  Arena mode bypassed the `gc_zero_ref_count_list` deferral (its head
  lives in base) and freed refcount-zero objects by direct recursion —
  one stack frame per object, which overflowed on a 100k-link WeakMap
  chain (test262 `staging/sm/regress/regress-1507322-deep-weakmap.js`).
  `JSRequestState` now carries a request-side zero-refcount worklist +
  `gc_phase` latch, drained iteratively by `free_zero_refcount_req` —
  the vanilla constant-stack discipline, rebuilt in request memory with
  zero base writes. Also a prerequisite for enabling the cycle
  collector on the hybrid-gc branch.
### Changed

- **⚠ Contract** (hybrid-gc branch) — the request region is now a
  reclaiming dlmalloc mspace instead of a bump arena. `js_free`
  actually frees post-freeze, so refcount-zero objects return their
  memory mid-request: the per-request allocation ceiling is now **peak
  live set (plus fragmentation)**, not cumulative allocation. Handlers
  that previously OOM'd on churn (large `JSON.parse` + transform
  pipelines) now run to completion in the same region size.
  Consumer notes:
  - `js_dual_arena_request_used()` now reports **live** bytes (it
    previously reported cumulative bump usage); it can go down.
  - `js_dual_arena_oom_used()` at a refusal likewise means live bytes —
    an OOM is now a genuine sizing signal rather than a churn artefact.
    `js_dual_arena_oom_limit()` reports full buffer capacity (it
    previously excluded the 16-byte cursor prefix).
  - Reset is still O(1) (fresh mspace header stomped over the dirty
    buffer; nothing freed or purged) and the "first post-reset
    allocation lands at the same address" invariant still holds —
    dlmalloc is deterministic for a fixed call sequence, and
    `JS_RelocateReqState` still aborts if that ever drifts.
  - New TU `qjs-dlmalloc.c` (vendored `dlmalloc.c` 2.8.6, unmodified)
    joins the runtime source set; static-linking embedders that list
    TUs explicitly must add it.

## [0.2.0] - 2026-06-14

Completes the **native host-callback surface**: a native driver (e.g. a
replay/simulation CLI with no JS host) can now both feed a recorded
request its inputs and observe its execution, using the same wire formats
the browser scrubber gets via `Module.tapes` / `Module.host_trace`.

### Added

- `arena_trace_set_host(on_event, on_state, user)` — native trace sink.
  In the browser build the trace emitter dispatches to `Module.host_trace`
  / `Module.host_state`; on a native build (no JS host) it now dispatches
  to C callbacks the embedder registers here. Same `kind` + payload wire
  format and same `0`/`1`/`2` return-code contract as the browser host,
  so a decoder written against `Module.host_trace` works on bytes captured
  natively. Declared in `qjs-arena-trace.h`, available only when the
  emitter is compiled in (`-DARENA_TRACE_ENABLED=1`). See the new
  `examples/arena_trace_native.c` for a complete decoder.
- `arena_replay_set_host(host, user)` — native replay sink (the input
  counterpart to the trace sink). In the browser build the replay bindings
  pull recorded values from `Module.tapes` via EM_JS imports; on a native
  build they now dispatch each tape read — `kv.get` / `kv.set` /
  `kv.delete` / `kv.prefix` and the module loader — to an
  `arena_replay_host` responder the embedder registers. Same outcome /
  divergence code contract as the browser host. With this plus
  `arena_trace_set_host`, `arena_set_date_now`, `arena_set_random_seed`,
  and the existing `arena_*` reactor entry points, a native driver has the
  complete hook surface to replay (and simulate) a recorded request with
  no JS host. Declared in `qjs-arena-replay-bindings.h`.
- `examples/arena_trace_native.c` / `arena_replay_native.c` + the
  `arena_trace_native` / `arena_replay_native` CMake targets (built under
  `-DQJS_BUILD_EXAMPLES=ON`): the output side (decode trace events) and the
  input side (serve module source + kv reads through `arena_replay_host`)
  of the native hook surface.

### Changed

- The trace-emitter host imports (`_arena_host_trace` / `_arena_host_state`)
  and the replay host imports (`_arena_host_kv_*`, `_arena_host_module_load`)
  now take their pointer arguments as real pointers rather than `int`
  addresses. On wasm32 this is identical on the wire (emscripten marshals
  each pointer to the same numeric address the `Module.*` host already
  received), so the browser contract is unchanged. The previous
  `(int)(intptr_t)` casts truncated 64-bit pointers, which is why native
  sinks were not previously possible.

⚠ **Contract:** the browser host wire formats (trace `kind`/payload,
snapshot JSON, return codes; tape outcome/divergence codes) are unchanged
— this is a MINOR addition of native-only entry points, not a break.

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
