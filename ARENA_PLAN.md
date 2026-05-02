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
6. **Inline caches.** Either disable IC for base-arena shapes, or store IC
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
- Currently on **step 1** (refcount inc/dec guard for base-arena objects).
