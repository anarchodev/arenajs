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

## [0.3.5] - 2026-08-16

**Per-statement source positions.** The compiler now anchors a source
position at every statement's first token, not only where an exception
could need one (calls, binary operators, `throw`, expression
statements). Previously a statement compiling to pure loads/stores —
`return v;`, `let x = 0;`, `const y = a;` — contributed no pc2line
entry, so anything resolving lines by pc (error attribution at those
pcs, and the trace's LINE events via `find_line_num`) attributed it to
the previous line; a replay scrubber could never step onto a bare-local
`return`. PATCH: no export, return-code, or wire-format change — trace
consumers simply see LINE events for statements that were invisible
before, and pc2line tables grow by the deduplicated per-statement
entries.

## [0.3.4] - 2026-07-08

The **base-surface release**: two additions that let an embedder (rove's
replay/sim engine) give the reactor base the same handler-global surface the
worker has — deferred-freeze construction so a `_system` prelude can be evaluated
into unfrozen base memory, plus native SHA-256/HMAC in the replay bindings.
MINOR: every existing export keeps its name, signature, and return codes.

- **MINOR: deferred-freeze reactor construction.** New exports
  `arena_reactor_new_open` (build without freezing), `arena_reactor_eval_base`
  (eval a classic script into the unfrozen base — for an embedder to install a
  `_system` prelude + composed shims that must live in base memory), and
  `arena_reactor_freeze` (idempotent seal). `arena_reactor_new` is unchanged
  (now = new_open + freeze). Lets rove's replay/sim engine give its base the
  same handler-global surface the worker has.
- **MINOR: native `crypto.sha256` / `crypto.hmacSha256`** in the replay
  bindings (embedded public-domain SHA-256, lowercase-hex over string/Uint8Array
  — matches the worker's `_system.crypto`). No OpenSSL, so `arenajs-replay` stays
  cross-compilable. Streaming sha256 + RSA/ECDSA remain out.

## [0.3.3] - 2026-07-08

The **multi-instance release**: the reactor becomes instance-based
(`ArenaReactor*`), with the singleton exports preserved verbatim as
wrappers over a default instance. MINOR, not MAJOR: every covered
export keeps its name, signature, return codes, and wire formats, and
the WASM export list is unchanged. The breaking changes below are
confined to trace-module internals that were never part of the covered
contract — see "Backward incompatibilities".

### Added

- **Instance-based reactor API** (`qjs-arena-reactor.h`, NEW header —
  the first header to declare the reactor surface; previously
  embedders hand-wrote `extern`s). `arena_reactor_new(base_kb,
  request_kb)` / `arena_reactor_free` create independent reactors, and
  `_r`-suffixed instance variants of the run/configure surface take
  native-width arguments (`arena_run_r`, `arena_run_module_r`,
  `arena_set_trace_mode_r`, `arena_set_request_mode_r`,
  `arena_set_random_seed_r(uint64_t)`, `arena_set_date_now_r(int64_t)`,
  `arena_oom_{hit,requested,used,limit}_r`). The existing singleton
  exports are unchanged in name, signature, and behavior — they are now
  thin wrappers over a default instance, and the WASM export list is
  byte-identical.
- **⚠ Contract — multiple reactors on one thread is now a stated,
  supported configuration** (harness + resettable sim), including runs
  *nested* across instances (a native callback fired during one
  instance's run may synchronously run modules on another). Trace
  state, determinism pins, OOM records, request-allocator regime, and
  entry-module resolution are all per-instance. What remains
  process-global, deliberately: `arena_trace_set_host` /
  `arena_replay_set_host` registrations are shared by all instances —
  a multi-instance host multiplexes in its callbacks (runs are
  synchronous, so it always knows whose run is active). Concurrent
  runs on different *threads* remain unsupported. Safe consumer
  pattern for trace events: scope the atom→string NAME map per
  run/instance — JSAtom ids are per-runtime, so a global map would
  silently mix instances.

### Changed

- Trace-emitter state (mode, NAME intern table, stop flag, active-event
  context, payload scratch) moved from process globals into a
  per-instance `ArenaTraceState`, bound to the traced runtime and
  resolved ctx→rt in each hook. Wire format and event semantics are
  untouched; nested runs across instances become correct by
  construction.

### Backward incompatibilities

None in the covered contract (exported `arena_*` functions, return
codes, build flags, wire formats — all identical; a rebuilt WASM
module has a byte-identical export list). The following break only
embedders that reached past the exported ABI into shipped-header
internals, in `ARENA_TRACE_ENABLED=1` builds:

- `arena_trace_set_mode(int)` and the `extern int arena_trace_mode`
  global are **gone** (there is no process-wide mode anymore). Use the
  exported `arena_set_trace_mode` / `arena_set_trace_mode_r`.
- `arena_trace_reset(void)` → `arena_trace_reset(ArenaTraceState *)`;
  `arena_trace_stop_armed(void)` →
  `arena_trace_stop_armed(const ArenaTraceState *)`. Both are reactor
  plumbing; embedders should not call them directly.
- `arena_entry_module(void)` → `arena_entry_module(JSContext *)` —
  previously an undeclared cross-TU extern consumed only by the replay
  bindings; now declared in `qjs-arena-reactor.h`.
- Behavioral, singleton path: determinism pins (`arena_set_random_seed`
  / `arena_set_date_now`) no longer survive an `arena_destroy` →
  `arena_init` cycle; they live in the instance and die with it. Pins
  are set per-request by every known driver, so this only affects a
  re-init that relied on inheriting stale pins.
  (`arena_set_trace_mode` before `arena_init` still works.)
- Behavioral, multi-instance hosts only: trace NAME atom ids are
  per-runtime — scope the atom→string map per run/instance. Existing
  single-instance hosts are unaffected.

## [0.3.2] - 2026-07-07

_Entry added retroactively in 0.3.3: v0.3.2 was tagged (at
`adc7356`) without a changelog cut — VERSION and build.zig.zon still
said 0.3.1 in that tree._

### Added

- `arena_set_request_mode(int mode)` reactor export (0 = GC mspace,
  1 = bump), also added to the WASM export list. Selects the
  request-allocator regime for subsequent runs; binds at the next
  request-arena reset, so call it between runs. Replay embedders use
  it to re-run a request under the regime the live request completed
  under (a GC-completed churny request would OOM under bump).

## [0.3.1] - 2026-07-07

Packaging fix: the 0.3.0 zig package omitted the GC-mode allocator
sources (`qjs-dlmalloc.c`, `qjs-dlmalloc.h`, `dlmalloc.c`) from
`build.zig.zon`'s `.paths` — `build.zig` compiled them but the fetched
package tarball didn't ship them, so downstream `zig fetch` consumers
of 0.3.0 fail with FileNotFound. No code changes; CMake/meson/artifact
consumers were unaffected.

## [0.3.0] - 2026-07-07

The **request-side GC release**: the per-request region gains a second,
default allocator regime — a dlmalloc mspace with real frees, refcount
reclamation, and a cycle collector with an automatic live-byte trigger —
so a handler's memory ceiling becomes its peak live set instead of its
cumulative allocation. The original bump regime remains available
per-reset (`js_dual_arena_set_request_mode`), enabling the
try-fast/retry-under-GC host pattern. The inviolate-base invariant is
now enforceable in production (`js_dual_arena_harden`: base goes
PROT_READ; the full 46,020-test corpus runs under it), and the last
known base-writers were closed on the way: prototype-chain shadow
visibility, setPrototypeOf identity checks, deep-teardown recursion,
snapshot Map/Set record locks, and the per-request determinism pins.
Resets stay O(1) in both regimes; JSRequestState moved to a fixed head
slot outside the allocators.

### Added

- **⚠ Contract** (hybrid-gc branch) — per-reset request-allocator
  selection: `js_dual_arena_set_request_mode(da, mode)` /
  `js_dual_arena_request_mode(da)` with `JS_ARENA_REQ_MODE_GC`
  (default: dlmalloc mspace, frees reclaim, refcount + cycle GC,
  ceiling = peak live set) and `JS_ARENA_REQ_MODE_BUMP` (bump cursor,
  free is a no-op, GC off, ceiling = cumulative allocation — master
  semantics, ~14% faster per request on alloc-heavy micro-benches).
  The choice takes effect at the NEXT reset; a request always runs
  entirely under one regime. `request_used`/`oom_used` follow the
  mode's semantics (live vs cumulative). Intended production pattern:
  run handlers on BUMP; on `oom_hit`, retry the request under GC and
  tag the handler churny. Mode state lives in the heap-side JSDualArena
  and JSRequestState — switching writes zero base bytes (validated by
  50 alternating hardened requests). Enabled by the fixed
  JSRequestState head slot, which makes rt->req independent of either
  allocator's layout.
- **`js_dual_arena_harden(da)` / `js_dual_arena_unharden(da)` /
  `js_dual_arena_is_hardened(da)`** — production enforcement of the
  inviolate-base invariant. After freeze, harden maps the base buffer
  `PROT_READ`; any write into it — engine bug, host misuse, anything —
  prints `[arena-harden] write to frozen base at base+<offset>` plus a
  backtrace (glibc) and dies with the default SIGSEGV action instead
  of silently drifting the snapshot. Where the thermometer measures
  and forgives, this is the MMU enforcing the invariant on every
  request. Mutually exclusive with the thermometer per arena.
  Discipline under harden: config APIs that write base
  (`JS_SetInterruptHandler`, `JS_SetGCThreshold`, ...) are pre-freeze
  only; per-request pins already land in `JSRequestState`; teardown is
  wholesale `js_dual_arena_free` (works while hardened) or unharden
  first. WASM / `ARENA_NO_THERM` builds return -1 (no mprotect).
  arena-smoke runs a full request hardened (base reads, shadowed
  writes, snapshot-collection iteration, pinned clock) and proves
  enforcement with a forked child whose raw base write dies by
  SIGSEGV.
### Changed

- **⚠ Contract** — the determinism pins (`JS_SetDateNow`,
  `JS_SetTimeOrigin`) now store per-request state in `JSRequestState`
  instead of the base-resident `JSContext`, completing the
  `random_state`/`interrupt_counter` relocation pattern. Two
  consequences for native embedders: pinning is no longer a base write
  (a per-request `arena_set_date_now` used to dirty the ctx page every
  request — poison for the shared-base/CoW future), and pins no longer
  leak across requests — `JS_ResetRequestArena` restores defined
  defaults (clock unpinned, origin 0, PRNG state zero). Call the
  setters AFTER the reset, before eval. The WASM reactor ABI is
  unchanged: `arena_set_random_seed` / `arena_set_date_now` remain
  sticky "set, then run" — the reactor buffers the latest values and
  re-applies them after its internal reset.

### Fixed

- **⚠ Contract** — closed the last known inviolate-base hole:
  iterating a Map/Set that lives in the snapshot wrote the iteration
  lock refcounts into base-resident map records (invisible to test262,
  which only builds request-side collections). Snapshot collections
  are now **readable and iterable forever, immutable after freeze**:
  the record-lock refcounts are skipped for base records (sound —
  immutability means no mid-iteration deletion to protect against),
  and every mutator (`set`/`add`/`delete`/`clear`/`getOrInsert`, all
  four collection classes) throws
  `TypeError: cannot mutate a frozen base collection; copy it first
  (e.g. new Map(m))` on a base receiver — except `getOrInsert` with a
  PRESENT key, which is a pure read-through and succeeds. Handlers
  needing a mutable copy: `new Map(snapshotMap)` (reads only).
  arena-smoke asserts iteration + reads + copy at zero base pages and
  the mutators throwing; the dedicated arena-basemap harness (from the
  base-collection-safety branch, which independently built this same
  fix first) covers the full matrix in the test-arena sweep.

- The WASM reactor ran **unseeded** after a host `arena_set_random_seed`:
  the seed landed in `JSRequestState` (relocated there for Math.random
  base-cleanliness), but `arena_run` / `arena_run_module` reset the
  request state FIRST, zeroing the PRNG before eval. The buffered
  re-apply above fixes it; the Date pin never hit this only because it
  still lived (wrongly) in base. arena-smoke now asserts the whole pin
  set dirties zero base pages.

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

- **⚠ Contract** (hybrid-gc branch) — `JS_RunGC` now runs the cycle
  collector on frozen arena runtimes (previously an unsafe walk,
  temporarily a no-op). It walks only the request-side registry; base
  objects are immortal leaves behind pointer guards. Cycles confined to
  request objects are reclaimed mid-request; cycles passing through a
  shadowed base object (e.g. hung off a monkey-patched base prototype)
  are a known blind spot — conservatively kept alive until request
  reset, exactly as today. Collection fires automatically: the trigger
  compares the allocator's LIVE byte count against a per-request
  threshold (seeded from `JS_SetGCThreshold`'s value at freeze, default
  256 KB; ratchets to 1.5x the surviving live set, floored at the
  seed). Because frees are real, acyclic churn never advances the live
  count — the threshold detects exactly cyclic accumulation, and
  handlers that build fewer cycles than the seed never pay a single
  collection. `-DFORCE_GC_AT_MALLOC` collects at every allocation for
  torture builds. Collection dirties zero base pages
  (thermometer-verified, incl. GC at every allocation over 4000 spec
  tests under ASan).

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
