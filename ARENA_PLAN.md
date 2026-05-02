# arenajs: One-Shot Per-Request JS VM Plan

Working plan for turning this clone of quickjs-ng into a JS VM optimized for
request-scoped server execution: no GC, bump allocator, per-request "restore"
collapses to a single bump-cursor write (~10 ns) instead of a memcpy.

## End State

- Two-arena model:
  - **base**: snapshot region. Holds runtime, prelude, prototypes, base atoms,
    base shapes. Allocated during snapshot build, never written again.
  - **request**: per-request region. Holds everything else. Reset between
    requests by writing one byte to the cursor.
- "Restore between requests" = `*(uint64_t *)request_arena.buf = 16`.
- The base arena can also be `mmap`'d shared across worker processes, but that
  is an optimization not required by the design.

### Inviolate base — the load-bearing invariant

The point of the design is that **no JS code can write to base, including
pathological user code**. `globalThis.X = ...`, `Array.prototype.foo = ...`,
`Object.defineProperty(Object, "x", ...)` — all of these must redirect to
request-arena allocations and leave base untouched.

This is the goal that justifies the entire CoW-thermometer regime. "Most
real workloads wouldn't do this" is not an argument — if any sequence of
JS calls can dirty a base byte, the snapshot isn't shareable across
workers, the bump-cursor reset isn't sound, and we've built a faster
version of the wrong thing.

Practical consequence: every chokepoint that touches a base allocation
(JSObject, JSShape, JSAtomStruct, JSString, JSContext) must either
short-circuit (read-only) or path-copy into the request arena. The
shape/atom overlays (steps 4 and 5) cover the property-table side of
this. Mutations of the base JSObjects themselves require a different
mechanism (shadowing or per-context request state) — see step 6.

## The Constraint That Shapes the Plan

Prior art at `~/src/rove/src/qjs/snap.zig` ships the memcpy-restore approach:
freeze the init image, memcpy it back per request, walk a relocation bitmap.
Cost: **5–7 µs per restore.** That is the floor for any approach that touches
every snapshot byte.

A `MAP_PRIVATE` CoW variant of the same idea was tried at rove and lost to
straight memcpy. Reason: per-request mutation is *scattered* across the
snapshot — refcount inc/dec, GC list pointer twiddles, atom refcounts on common
atoms — so CoW faults and duplicates page after page. By the end of a request
a large fraction of the snapshot is materialized in writable pages and the
fault tax exceeds the memcpy.

The same scattered mutation is what makes a true "cursor reset = restore"
infeasible today. To get below 5–7 µs we have to attack the mutation, not the
copy.

## Strategy

Kill base-arena mutation sources progressively. Use CoW restore time as a
**thermometer**: it measures how many distinct pages of base were touched per
request. Each mutation source we eliminate should monotonically reduce CoW
cost. When CoW cost approaches the page-fault tax for a tiny handful of pages,
base is effectively read-only and we switch to the cursor-reset path.

This converts an open-ended audit ("did we get every mutation site?") into a
continuous measurable variable.

## Mutation Sources, in Kill Order

1. **Refcount inc/dec on base objects.** Biggest by far — pervasive, hot. Add
   a pointer-range guard at the inc/dec chokepoint: if the GC object is in the
   base arena, no-op. Base objects become immortal. Half a day's work.
2. **GC list manipulation on base objects.** Same kind of guard — if the
   object's arena membership is base, skip list insertion / removal /
   `gc_obj_list` linkage updates.
3. **Per-request mutable runtime state.** `current_exception`,
   `current_stack_frame`, `gc_phase`, `malloc_state` counters, `job_list` head,
   active context — all live on `JSRuntime`, which lives in base. Move into a
   `JSRequestState` struct allocated at the start of the request arena,
   indirected through `rt->req`. Mechanical sed-style refactor.
4. **Shape transitions discovered during a request.** New transitions can't
   write the parent (base) shape's `children` slot. Need a "transition
   overlay" stored in the request arena: lookup checks overlay first, then
   falls back to the in-place pointer.
5. **Atom interning of new atoms.** Two-tier atom table: base atoms read-only,
   request atoms in request arena, lookup falls through.
6. **Mutations to base JSObjects themselves.** Pathological user code can do
   `globalThis.X = ...`, `Array.prototype.foo = ...`, `Object.defineProperty(...)`
   etc. Even with steps 4 and 5, the property-set chokepoint still writes into
   the JSObject in base (`p->shape = new_sh`, `p->prop = new_prop`, and the
   `pr->u.value = val` write itself if `p->prop` wasn't reallocated). Two
   complementary sub-fixes:
   a. **Per-context request state** — `ctx->creq` indirection like `rt->req`.
      Move per-request mutable ctx fields (current global var bindings,
      class_proto overrides, std_array_prototype flag, etc.) into a struct
      in the request arena. Catches mutations rooted at ctx (the common case).
   b. **Shadow-on-write for arbitrary base JSObjects** — when a write would
      modify a base JSObject, allocate a fresh request-arena copy, register
      it in a per-request shadow map (base p → request shadow), and route
      future references via the map. Reads on unshadowed base objects stay
      direct. This is the fully general fix and the one the original
      "immutable internal data structures" framing pointed at; full
      persistent-data-structure semantics for the JS object graph.
7. **Inline caches.** Either disable IC for base-arena shapes, or store IC
   entries in a request-arena side table.

## Prerequisites

- [x] **0a. Dual bump arena, no-op free, last-alloc realloc fast path.**
  Files: `qjs-arena.h`, `qjs-arena.c`. Smoke test: `arena-smoke.c`.
  `JS_NewRuntimeArena`, `JS_FreezeRuntime`, `JS_ResetRequestArena`,
  `JS_GetDualArena`, `JS_GetMallocOpaque` exposed.
- [x] **0b. Switch each arena to a single preallocated buffer.** Drop the
  chunk list. Cursor lives at offset 0 inside the buffer (rove trick — keeps
  pointers self-relocating if we ever add snapshot persistence). Reset
  becomes `*(uint64_t *)buf = ARENA_PREFIX_LEN`.
- [x] **0c. Apply the rove determinism patch.** `random_state = 0` by default
  with `JS_SetRandomSeed` to inject post-restore. `time_origin = 0` by default
  with `JS_SetTimeOrigin`. `performance.timeOrigin` becomes a getter reading
  `ctx->time_origin` live so a single setter call updates both surfaces.
  Reference: `~/src/rove/vendor/quickjs-ng/rove-kv-deterministic-init.patch.md`.

## Validation Loop

After each mutation source killed:
1. Run a representative request workload N times under `MAP_PRIVATE` snapshot
   restore. Measure mean per-request restore time.
2. Compare to the previous measurement. Should drop monotonically.
3. If a step does not move the needle, stop and investigate — the mutation is
   coming from somewhere we did not predict, and continuing the list will not
   help.

When restore time approaches the bare page-fault tax for ≤2–3 distinct pages,
switch the public restore path to bump-cursor reset and retire the CoW path.

## Reference

- Memcpy-restore + relocation-bitmap implementation:
  `~/src/rove/src/qjs/snap.zig`
- Determinism patch (the volatile-slot list and the fix for each):
  `~/src/rove/vendor/quickjs-ng/rove-kv-deterministic-init.patch.md`

## Status

- 0a complete.
- 0b complete. Smoke test still passes; per-allocation overhead dropped a touch
  (chunk header gone). `arena_reset()` is now three stores: cursor + two
  last-alloc fields (the cursor is the only one that affects correctness; the
  others just disable the realloc-extend-in-place fast path until refilled).
- 0c complete. `Math.random()` returns 0 unseeded, deterministic from seed
  after `JS_SetRandomSeed`. `performance.timeOrigin` returns 0 unset,
  whatever `JS_SetTimeOrigin` wrote afterwards. Two consecutive runs of the
  smoke test produce identical `base_used` numbers, which is the necessary
  (not yet sufficient) condition for byte-deterministic init.
- 1 (chokepoint guard) partial. `js_dup` and `JS_FreeValueRT` now no-op when
  the value's payload pointer is in the base arena. The four control-flow
  `ref_count == 1 / != 1 / > 1` sites (rope cache, string in-place concat,
  two shape clone-if-shared paths) also OR-in `js_arena_ptr_is_base` so base
  objects always take the safe (clone) path. Mechanism: process-global
  `(js_arena_base_lo, js_arena_base_hi)` set by `js_dual_arena_freeze`. One
  arena-runtime per process; documented in `qjs-arena.h`.
- Thermometer in place. Both arenas now mmap-backed (page-aligned).
  `js_arena_thermometer_*` mprotects base read-only and counts SIGSEGV
  faults; the handler marks each dirtied page in a bitmap and makes the
  page writable so the faulting instruction can succeed. Reset re-protects
  the entire base region in one syscall.

  **Baseline measurement** (post-chokepoint guard, before internal-site
  guards) on the smoke-test workload:
  - request#1 (loop + reduce + closure): 12 base pages dirtied, 12 writes
  - request#2 (loop + object literals): 7 base pages dirtied, 7 writes

  writes == pages because each first-write makes the page writable for the
  rest of the request. So the metric is unique-pages-dirtied; CoW would
  copy that many pages per request.

- 1b complete. `arena_rc_inc(hdr)` / `arena_rc_dec(hdr)` inline helpers
  applied to the internal direct-touch sites: atom inc/dec (`JS_DupAtom`,
  `JS_DupAtomRT`, the two atom-hash hit paths, `__JS_FreeAtom`, the
  string-dec inside the atom-intern path), shape inc/dec (`js_dup_shape`,
  `js_free_shape`), `js_free_string`, `free_var_ref`. GC suppressed in
  arena mode (cycle collector walks `gc_obj_list` and writes ref_counts
  directly, would underflow our deliberately-small base ref_counts).
  `js_calloc_rt` / `js_malloc_rt` / `js_realloc_rt` / `js_free_rt` skip
  malloc_state tracking in arena mode — its only consumer was the GC
  threshold and we'd be writing those counters into base on every alloc.

  **Thermometer after 1b:**
  - request#1: 12 → 9 base pages dirtied (saved 3)
  - request#2:  7 → 5 base pages dirtied (saved 2)

  Remaining writes look like genuine runtime-state mutation: assignments
  to `rt->current_exception`, `rt->current_stack_frame` push/pop,
  `gc_obj_list` link operations when new request-side GC objects are
  inserted, `shape_hash` table writes when new shapes are linked. None
  of these are refcount work — they're the runtime book-keeping fields
  that step 2 (relocate to per-request state) targets.

- 2a partial: skipped `gc_obj_list` link/unlink in arena mode
  (`add_gc_object` initializes a self-loop link instead, `js_free_value_rt`
  bypasses the `gc_zero_ref_count_list` queue and calls `free_gc_object`
  directly). Skipped `JS_FreeAtomStruct` in arena mode (would otherwise
  rewrite `rt->atom_array[i]`, `rt->atom_count`, `rt->atom_free_index`
  and traverse `rt->atom_hash` chains — all in base).

  Thermometer:
  - request#1: 9 → 8 base pages dirtied (saved 1)
  - request#2: 5 → 5 (unchanged this run)

  Marginal drop because the eliminated bytes overlap with pages already
  dirtied by other writes (probably the JSRuntime struct itself, which
  spans a few pages and gets mutated for `current_exception`,
  `current_stack_frame`, `shape_hash`, etc.).

- Diagnostic done. `js_arena_thermometer_dirty_offsets` plus
  `JS_DumpRuntimeOffsets` give per-page byte offsets and the layout map.

  Residual 8 pages (request#1) split cleanly:
  - **+0** — JSRuntime struct itself (current_exception, current_stack_frame,
    list heads, malloc_state, atom_hash buffer start). Target of step 2.
  - **+4096** — middle of `atom_array` backing buffer. Written when a new
    atom is interned during a request. Target of step 5 (atom overlay).
  - **+20480, +24576, +28672** — class_array, shape_hash buffer, JSContext
    + class_proto array. shape_hash[h] is written when a new shape
    transition is registered. Target of step 4 (shape overlay).
  - **+49152, +53248, +57344** (request#1 only) — base-arena snapshot
    allocations the closure path mutates. Need to find what slips past
    the chokepoints; closure creation involves bytecode reference count
    bumps (`b->header.ref_count++`) at sites we haven't guarded yet.

- 2 + chokepoint cleanup, including:
  - `JSRequestState` move (current_exception, current_stack_frame,
    in_out_of_memory, in_build_stack_trace, parent_promise).
  - `ctx->interrupt_counter` decrement skipped in arena mode
    (was a per-bytecode-op base write).
  - `rt->atom_count++/--`, `rt->shape_hash_count++/--` skipped in arena
    mode (counters with no consumer once GC is suppressed).
  - **Unhashed-base-shape clone fix** in both `add_property` and
    `js_shape_prepare_update`. The existing clone-if-shared-or-base
    branch sat inside `if (sh->is_hashed)`, so unhashed base shapes fell
    straight through to `add_shape_property` and got mutated. Both
    callers now have a parallel else-if branch that clones unhashed
    base shapes.
  - Thermometer gained a byte-level signal
    (`js_arena_thermometer_changed_bytes`,
    `_changed_in_page`, `_changed_byte_offsets`,
    `_baseline_at`) plus a SIGSEGV-handler `trace_range` that
    backtraces faults inside a chosen offset window — used to identify
    the unhashed-shape mutation site.

  **Thermometer (steady-state request#2) so far:**
  | source        | offset    | bytes |
  |---------------|-----------|-------|
  | rt page       | +0        | 7     |
  | atom_array    | +4096     | 18    |
  | shape_hash    | +20480    | 6     |
  | global_var_obj prop | +28672 | 14 |
  | **total**     |           | **45** |

  Pages: 4 (down from 7). Bytes: 45 (down from ~120 baseline). The four
  remaining pages each have one structural cause:
  - **+0 (7 B)** — `rt->atom_free_index` (intern path) plus 3 new-atom
    `atom_hash[h]` slots. Targets atom overlay (step 5).
  - **+4096 (18 B)** — `atom_array[i] = new_atom` writes. Atom overlay.
  - **+20480 (6 B)** — `shape_hash[h] = new_shape` writes. Shape
    overlay (step 4).
  - **+28672 (14 B)** — `JS_DefineGlobalVar` writes a new property's
    `pr->u.value` into `ctx->global_var_obj->prop[i]`, where both `p`
    and `p->prop` are in base. This is exactly the case step 6
    addresses: any JS code that adds a property to a base JSObject
    (top-level `let`, `globalThis.X = ...`, prototype monkey-patching,
    etc.) currently mutates base. Not a smoke artefact — a real
    correctness gap against the "no JS code can write to base"
    invariant.

- Currently on **step 4 (shape overlay)** — relocate request-discovered
  shape transitions and their `shape_hash[h]` chain entries into the
  request arena. Eliminates the +20480 page entirely and makes shape
  table mutations safe under reset. Step 6 is the bigger structural
  change that closes the global-object hole.
