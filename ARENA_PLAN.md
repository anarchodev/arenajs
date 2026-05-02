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
this. Mutations of the base JSObjects themselves require shadow-on-write
(step 6).

### Snapshot stays maximal — the symmetric invariant

The other side of the invariant: **as much of the runtime as possible
stays in the immutable snapshot**. JSRequestState (and any future
`ctx->creq`) is for the genuinely tiny per-request mutable bits — flags,
the current exception slot, a stack-frame pointer, the shadow-map root.
It must not become a junk drawer for relocating large structures out of
base. If we move atom_array, class_proto, the global object, etc. into a
per-request struct, every request reallocs/copies them and we've defeated
the "shareable immutable snapshot" half of the design.

Concretely: when a step finds new mutation sources, the question is
"can I make the existing structure read-only and path-copy on write?"
not "can I move this whole structure into per-request state?" Path
copying keeps the unchanged spine in base; bulk relocation duplicates
the whole thing. Always prefer the former.

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
   `pr->u.value = val` write itself if `p->prop` wasn't reallocated).

   **Shadow-on-write for arbitrary base JSObjects.** When a write would modify
   a base JSObject, allocate a sparse copy in the request arena, register it
   in a per-request shadow map (base p → request shadow), and route subsequent
   reads/writes for that object via the map. Reads on unshadowed base objects
   stay direct (no map lookup overhead). This is the persistent-data-structure
   form from the original conversation — Clojure-style path copying, not bulk
   copying.

   **Don't bulk-relocate ctx fields.** It's tempting to add a
   `JSContextRequestState` and dump `global_obj`, `global_var_obj`,
   `class_proto[]`, etc. into it; that path leads to copying kilobytes per
   request and defeats the whole "snapshot is the shared, immutable bulk"
   model. The shadow map is the right shape: sparse, allocated only for the
   handful of objects a given request actually mutates. The map root and
   any genuinely tiny per-request ctx state (`std_array_prototype` flag,
   etc.) can live in a small `ctx->creq` indirection — the same discipline
   as `rt->req`: kept small, *not* a junk drawer for moving stuff out of
   the snapshot.
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

- 4 done. `JSRequestState` gained a small `shape_overlay` (pointer +
  size + count, lazily allocated). `js_shape_hash_link/unlink` route
  request-arena shapes into the overlay and leave the base
  `rt->shape_hash` table untouched. `find_hashed_shape_proto` and
  `find_hashed_shape_prop` walk the overlay first, then fall through to
  base. While doing this, also fixed two related residual base writes:
  `JS_DupContext` / `JS_FreeContext` were doing direct `ctx->header.ref_count`
  inc/dec (now routed through `arena_rc_inc/dec`), and
  `__JS_EvalInternal` was clearing `ctx->error_back_trace` to
  JS_UNDEFINED unconditionally on every top-level eval — even when
  already undefined — which faulted the page without changing bytes
  (now skipped if already undefined). Also gated the direct
  `list_del`/`list_add_tail(&sh->header.link, &ctx->rt->gc_obj_list)`
  pairs in `resize_properties`.

  **Page +20480 is gone.** Steady-state request#2:
  | source        | offset    | bytes |
  |---------------|-----------|-------|
  | rt page       | +0        | 7     |
  | atom_array    | +4096     | 18    |
  | global_var_obj prop | +28672 | 14 |
  | **total**     | 3 pages   | **39** |

- 5 done. JSRequestState gained `atom_overlay` (request-arena slot
  array), `atom_overlay_base` (= base atom_size at freeze; UINT32_MAX
  pre-freeze so the overlay path is dead), `atom_hash_overlay`, and
  small bookkeeping fields. New atoms interned during a request get
  index >= atom_overlay_base and live in the overlay; lookups dispatch
  via `js_atom_struct(rt, i)`. `__JS_NewAtom` and `__JS_FindAtom` walk
  the overlay hash chain first then fall through to the base chain.
  `js_get_atom_index` walks the overlay or base chain depending on
  whether `p` is in the base arena.

  Mechanical work: `rt->atom_array[X]` reads (~30 sites) bulk-replaced
  with `js_atom_struct(rt, X)` via sed. Three LHS write sites in
  init/intern/free reverted to direct base access (those are pre-freeze
  init or the gated free path).

  **Steady-state request#2: 1 page, 14 bytes.** Only +28672 remains —
  the `JS_DefineGlobalVar` write into `ctx->global_var_obj->prop[i]`.
  Pages +0 and +4096 (atom-related) are gone.

- 6 done for `ctx->global_var_obj` — the only base JSObject the smoke
  exercises via `let` declarations. Mechanism:
  - `JSContextRequestState` (creq) struct holds sparse JSValue
    shadows. Single field for now (`shadow_global_var_obj`); space
    for adding more as the thermometer surfaces them. NOT a junk
    drawer — large structures stay in base.
  - creq nodes live in a linked list rooted at `rt->req->creq_list`
    (since ctx itself is in base and we can't dirty it to stash a
    creq pointer). `js_creq(ctx)` walks the list — typical use has
    1–2 contexts.
  - `js_clone_jsobject_for_write` shallow-copies a base JSObject into
    the request arena: header reset (ref=1, self-loop link, no weak
    refs), shape shared (immutable), prop array copied. Property
    values stay shared with base; base values are immortal so no
    refcount inc.
  - `js_global_var_obj_active` returns shadow if present else base
    (read path). `js_global_var_obj_for_write` lazily creates the
    shadow on first mutation (write path).
  - All ~6 access sites (`JS_DefineGlobalVar`, `JS_GetGlobalVar`,
    `JS_GetGlobalVarRef`, `JS_SetGlobalVar`, `JS_DeleteGlobalVar`,
    `JS_CheckDefineGlobalVar`) routed through these helpers.

  Plus two satellite fixes:
  - `assert(atom < rt->atom_size)` replaced with
    `assert(js_atom_in_range(rt, atom))` (overlay atoms are now valid
    indices >= base atom_size).
  - `list_add_tail(&link, &rt->string_list)` under `ENABLE_DUMPS` (debug
    builds only) gated to self-loop in arena mode, mirroring
    `add_gc_object`. Production NDEBUG builds don't compile this in.

  And one cleanup: the smoke test no longer calls `JS_FreeContext` /
  `JS_FreeRuntime` on shutdown. Arena mode has no per-allocation
  teardown — the whole arena vanishes via `js_dual_arena_free`'s
  munmap. The refcount-based teardown walks (`assert(list_empty(...))`,
  per-atom `JS_FreeAtomStruct`, etc.) only make sense for the default
  path.

  **Steady-state request#2: 0 base pages, 0 bytes — under both Release
  and Debug+ASan.**

- 6 v2 + reset audit done. The single-target shadow_global_var_obj
  (v1) is gone; in its place a generic `shadow_map` (linked list of
  base p → request shadow p) lives on `JSRequestState`. The
  property-access chokepoints redirect base targets to shadows:
  - **Writes**: `JS_SetPropertyInternal2`, `JS_DefineProperty` —
    `js_object_for_write(ctx, p)` lazily creates a shadow on first
    write. The `obj` and `this_obj` JSValues are swapped together when
    they reference the same base.
  - **Reads**: `JS_GetPropertyInternal` plus the inline fast paths
    `OP_get_field` / `OP_get_field2` in the bytecode interpreter —
    `js_object_active(rt, p)` returns the shadow if one exists, else
    base. Pre-freeze and request-arena pointers pass through with
    branch-only overhead.
  - The v1 global_var_obj-specific helpers stay as thin wrappers over
    the generic mechanism so the existing `let`/`var` call sites
    (`JS_DefineGlobalVar`, `JS_GetGlobalVar`, etc.) continue to work.

  Reset audit: `JS_ResetRequestArena` rewinds the cursor and
  immediately calls `JS_RelocateReqState`, which `js_mallocz_rt`'s a
  fresh `JSRequestState` as the very first post-reset allocation.
  Because the cursor is at PREFIX_LEN, the new struct lands at the
  same address as the original — `rt->req` (the one base write at
  freeze) remains valid; an `abort()` guards the invariant. All
  per-request runtime caches (shape_overlay, atom_overlay,
  shape_map, …) live inside `JSRequestState` (or are pointed at from
  it) and are cleared by re-init. No stale request-arena pointers
  remain in base after reset.

  **Reset benchmark** (`arena-bench.c`) — 50000 iterations of:
  > `if (globalThis.blah !== undefined) throw 'leak';
  >  globalThis.blah = 42;
  >  if (globalThis.blah !== 42) throw 'set did not stick';
  >  'ok'`
  followed by `JS_ResetRequestArena`:
  - **Full eval + reset**: ~3.2 μs/iter (Release).
  - **Reset alone**: **~9 ns/iter** (Release).

  The 9 ns figure is essentially `arena_set_cursor` + `js_mallocz_rt`
  for the JSRequestState (104 bytes, zeroed) + a couple of field
  initializations. Versus the rove memcpy-restore baseline of 5–7 µs
  per restore, the bump-cursor reset is ~700× faster.

- Currently: **goal achieved end-to-end** for the smoke and benchmark
  workloads. The generic shadow_map covers every base JSObject any
  property write could target. Open work is mostly hardening:
  - Other property-mutation chokepoints (`JS_DeleteProperty`,
    `JS_SetPrototypeInternal`, etc.) need the same redirect for
    completeness against arbitrary user code.
  - Larger benchmark workloads will surface the next residual writes
    (atom-overlay growth, shape transitions on shadows that escape
    add_property, etc.).
