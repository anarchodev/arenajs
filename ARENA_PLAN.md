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

## Classes of base-mutation we've had to fix

Every fix so far has fallen into one of a small number of patterns.
Listed here so future investigation can be systematic instead of
reactive — search the codebase for each pattern and apply the
corresponding remedy, instead of waiting for the thermometer to
surface them one at a time.

### 1. Refcount inc/dec on a base object

**Symptom:** every dup / free of a base value writes to the object's
ref_count field in base.

**Remedy:** chokepoint guard. `js_dup` and `JS_FreeValueRT` test
`js_arena_ptr_is_base` and short-circuit. Internal direct `ref_count++`
and `--ref_count` sites use the `arena_rc_inc` / `arena_rc_dec`
inline helpers.

**Where to look next:** any direct `header.ref_count++`/`--` not
already going through the helpers. Candidates not yet audited:
`b->header.ref_count++` for JSFunctionBytecode at module-load sites
(~line 29950, 35532, 38100, 38157), `JSAsyncFunctionData` ref bumps
(line 20695-ish), JSBigInt/JSStringRope ref ops, `JSModuleDef` ref
counting.

### 2. `ref_count == 1` "uniquely owned, mutate in place" checks

**Symptom:** code reads `ref_count == 1` or `!= 1` and skips the
clone path because "we own this." For an immortal base object whose
ref_count is whatever it was at freeze (often 1), the check
mistakenly fires and mutates base.

**Remedy:** OR `js_arena_ptr_is_base(p)` into the check so base
objects always take the clone path.

**Where to look next:** `grep -nE 'ref_count == 1|ref_count != 1|ref_count > 1'`
for any sites we missed. Currently fixed: rope cache update, string
in-place concat, two shape clone-if-shared paths (`add_property`,
`js_shape_prepare_update`).

### 3. Per-op / per-call counters whose only consumer is GC or
resize thresholds

**Symptom:** something decrements a counter on every bytecode op or
every allocation; the counter exists only to trigger a periodic
operation we've disabled in arena mode.

**Remedy:** skip the decrement in arena mode.

**Already fixed:** `ctx->interrupt_counter` (per bytecode op),
`rt->atom_count`, `rt->shape_hash_count`, `malloc_state.malloc_size`
+ `malloc_count` tracking in `js_*_rt`.

**Where to look next:** any other "trigger every N ops" mechanism.
`js_poll_interrupts` is the obvious one we've already done; might be
more in promise/job queue processing.

### 4. Per-request mutable state on `JSRuntime` / `JSContext`

**Symptom:** small fields written by request execution that live on
the immortal base struct.

**Remedy:** relocate to `JSRequestState` (on rt, lives in request
arena post-freeze) for runtime-wide bits, or to per-context state
(currently the shadow_map mechanism subsumes this) for ctx-rooted
bits. Discipline: keep these structs *tiny* — only flags/pointers,
never large structures.

**Already fixed (`JSRequestState`):** `current_exception`,
`current_stack_frame`, `in_out_of_memory`, `in_build_stack_trace`,
`parent_promise`.

**Where to look next:** `ctx->error_back_trace`,
`ctx->error_prepare_stack`, `ctx->error_stack_trace_limit`,
`ctx->std_array_prototype` flag, `ctx->binary_object_count` /
`binary_object_size` (per-context stats). Any list head on rt that
gets `list_add`/`list_del` per request: `job_list`,
`loaded_modules` (on ctx).

### 5. List-head writes for tracking that nothing reads in arena mode

**Symptom:** code calls `list_add_tail(&node, &rt->some_list)` on
every allocation (or freeing), purely so a later walk can find them.
With GC suppressed and no teardown walks, the list is write-only.

**Remedy:** skip the link in arena mode and `init_list_head`
the node so subsequent `list_del` is a safe no-op. Mirrors what
`add_gc_object` does.

**Already fixed:** `gc_obj_list`, `gc_zero_ref_count_list`,
`tmp_obj_list`, `string_list` (under ENABLE_DUMPS), the inline
`list_del`/`list_add_tail` pairs in `resize_properties`.

**Where to look next:** `rt->job_list`, `ctx->loaded_modules`,
`rt->context_list`. Plus any `#ifdef ENABLE_DUMPS` site we missed.

### 6. Required-mutation hash tables (shape_hash, atom_array,
atom_hash)

**Symptom:** lookup needs to find new entries written during a
request, so the table can't just be skipped — it has to grow.

**Remedy:** per-request **overlay** in the request arena. Lookups
check overlay first then fall through to the immortal base table.
Inserts go into overlay only; base is never written.

**Already fixed:** shape_hash (overlay on `JSRequestState`), atom
table (atom_overlay + atom_hash_overlay, with index threshold
dispatch via `js_atom_struct`).

**Where to look next:** any other base hash table. Candidates:
weak-ref tracking (`p->first_weak_ref` chain), module name table,
inline caches if/when added.

### 7. Property mutation on base JSObjects

**Symptom:** any JS code path that writes/deletes a property,
defines a property, or sets prototype on a base JSObject mutates
base via `p->shape = ...`, `p->prop = ...`, `pr->u.value = ...`.

**Remedy:** **shadow-on-write**. `js_object_for_write(ctx, p)`
allocates a sparse request-arena copy on first mutation and
registers it in `rt->req->shadow_map`. `js_object_active(rt, p)`
returns the shadow on subsequent reads. Each property-mutation
chokepoint must redirect at the entry; reads via the bytecode fast
path also need redirect.

**Already hooked:** `JS_SetPropertyInternal2`, `JS_DefineProperty`,
`JS_DeleteProperty`, `JS_SetPrototypeInternal`, `OP_get_field`,
`OP_get_field2`, `OP_put_field`. v1 wrappers
(`js_global_var_obj_active` / `_for_write`) for the global_var_obj
sites.

**Where to look next:** **`JS_DefineGlobalVar`'s var-on-`global_obj`
branch (line 11811)** and `JS_DefineGlobalFunction` still
short-circuit `add_property` directly on base `global_obj`. Other
candidates: `OP_define_field`, `OP_set_array_el`, `OP_put_array_el`,
exotic property setters (`JSClassExoticMethods.set_property`,
`define_own_property`), `Object.assign` internals,
`Object.preventExtensions`, `Object.freeze`.

### 8. Lazy initialization that fires post-freeze

**Symptom:** a property (or value) marked "initialize on first
access" gets touched during a request, and the init code mutates
base.

**Remedy:** **pre-force at snapshot build**. `JS_ForceAllAutoinit`
walks `rt->gc_obj_list` and instantiates every `JS_PROP_AUTOINIT`
property before the dual arena flips. Iterates to fixpoint.

**Already fixed:** `JS_AutoInitProperty` (all four IDs:
PROTOTYPE, MODULE_NS, PROP, BYTECODE).

**Where to look next:** any other "pay on first use" path.
Candidates: lazy module loading
(`JS_GetImportMeta` / `js_resolve_module`), lazy regex compilation
(`ctx->compile_regexp` first invocation), bytecode lazy resolution
in `js_bytecode_autoinit` (already covered by autoinit but worth
verifying), promise-job queue init.

**Principle:** lazy init is an antipattern for snapshot-based
runtimes. Anything that *can* be init at snapshot-build time
*should* be — cost amortizes across mmap'd workers and per-request
latency variance drops.

### 9. Same-value "defensive" writes

**Symptom:** code writes a value that's almost always already that
value (e.g. clearing a slot to `JS_UNDEFINED` at the start of every
top-level eval). The page faults under `mprotect` even when the
byte content doesn't actually change.

**Remedy:** test before write. `if (!already_clear) clear`.

**Already fixed:** `__JS_EvalInternal` clearing
`ctx->error_back_trace`.

**Where to look next:** other "reset to default" sites in error
paths, exception handling, finalizer setup, eval entry.

### 10. Teardown / cleanup walks that mutate base

**Symptom:** `JS_FreeRuntime` / `JS_FreeContext` walk lists, free
each entry, decrement counters — all on base structures.

**Remedy:** in arena mode there is no per-allocation teardown; the
whole arena vanishes via `js_dual_arena_free`'s munmap. Either
skip the teardown walks or don't call `JS_FreeRuntime` at all.
Smoke test currently does the latter.

**Already handled:** `JS_FreeAtomStruct` returns early in arena
mode; `js_*_rt` skip malloc_state tracking; `js_free_value_rt`'s
OBJECT branch bypasses the `gc_zero_ref_count_list` queue.

**Where to look next:** if anyone ever calls `JS_FreeContext` /
`JS_FreeRuntime` in arena mode (they shouldn't), audit the walks.

### 11. ENABLE_DUMPS / debug-build paths

**Symptom:** code under `#ifdef ENABLE_DUMPS` writes to base data
structures (typically list-head appends for leak tracking). Fires
in Debug + ASan builds.

**Remedy:** gate with `if (js_arena_base_lo)` and self-loop the
link instead.

**Already fixed:** the five `list_add_tail(&str->link,
&rt->string_list)` sites.

**Where to look next:** other `#ifdef ENABLE_DUMPS` sections —
JS_DUMP_FREE, JS_DUMP_GC, JS_DUMP_ATOMS, JS_DUMP_SHAPES,
JS_DUMP_OBJECTS, JS_DUMP_MEM. Most are read-only walks, but worth a
sweep.

### 12. Wall-clock / nondeterministic init

**Symptom:** init reads system time / random source, embedding
nondeterministic values into base. Breaks both byte-deterministic
snapshots (rove-style) and the inviolate-base goal (any later
re-set would be a base mutation).

**Remedy:** zero-default in init + caller-supplied setter post-init
(`JS_SetRandomSeed`, `JS_SetTimeOrigin`) + live getters where the
JS-visible value derives from a per-request input.

**Already fixed:** `random_state`, `time_origin`,
`performance.timeOrigin` (rove determinism patch).

**Where to look next:** locale-dependent state (`Date` zone offsets,
`Intl.*` if/when added), HRNG-derived values in any future
crypto-related init, environment-variable reads at init time.

### 13. Control-flow branches that miss a code path

**Symptom:** existing logic handles "the common case" but a less
common branch falls through to a path that mutates base.

**Remedy:** find the missed branch, add a parallel handler. Tends
to be subtle — the smoke test silently passes because the common
path works.

**Already fixed:** unhashed-base shape clone in `add_property` and
`js_shape_prepare_update` (the `is_hashed` branches handled
clone-if-base; the unhashed-base case fell straight through to
`add_shape_property` and mutated base).

**Where to look next:** every property-mutation chokepoint —
recheck whether all branches go through a redirect. Especially
`add_property` interactions with exotic objects, fast-array paths,
proxy targets.

### How to use this list

When the thermometer surfaces a new base write, before designing a
fix, check which class it falls into — the remedy is usually
already established. New classes are themselves a finding worth
documenting here. The list also points at "places we haven't
investigated yet" — running through it systematically, even before
the thermometer complains, would catch the next class of bugs
proactively rather than reactively.

### Audit pass — closed in commits d5c451c..c65ff4d

Walked through the "where to look next" candidates and applied the
established remedy in each case (one commit per fix):

- **Class 7**: `JS_DefineGlobalVar`'s var-on-`global_obj` branch
  routed via `js_object_for_write` (top-level `var` writes).
- **Class 7**: `JS_DefineGlobalFunction`'s redeclaration check
  routed via `js_object_active` so prior-shadow defines are visible.
- **Class 1**: 6 var_ref + 1 bytecode + 1 string-slice
  `ref_count++` and 1 async-function `--ref_count` migrated to
  `arena_rc_inc / _dec`.
- **Class 4**: `ctx->std_array_prototype` shadowed via per-request
  `std_array_prototype_dirty` flag on JSRequestState; reads use a
  helper that AND's both. `ctx->binary_object_count` /
  `binary_object_size` (pure stats) skipped in arena mode.
- **Class 5**: `rt->job_list` relocated to `JSRequestState`
  (`init_list_head` re-run by `JS_RelocateReqState` after the
  memcpy because list heads are self-referential).
  `ctx->loaded_modules` add skipped in arena mode; module
  re-resolution per request is the trade.
- **Class 7**: `OP_put_array_el`'s two fast paths bypass for base
  arrays; falls through to `JS_SetPropertyValue` →
  `JS_SetPropertyInternal2` (already hooked).

**Bench after sweep:**
| | Release | ASan |
|--|--|--|
| A: globalThis.blah set      | 3.7 µs | 31 µs |
| B: override base GREETING   | 3.5 µs | 30 µs |
| C: delete base GREETING     | 3.5 µs | 29 µs |
| D: 100x push + reduce       | 20 µs  | 199 µs |
| reset alone (floor)         | **14 ns** | 66 ns |

Reset alone went from 8 → 14 ns: extra `init_list_head` for the
job_list head on each `JS_RelocateReqState`. Worth it.

Still open from the catalog: lazy-module instantiation
(class 8), other ENABLE_DUMPS sites if any (class 11),
JS_FreeContext path (class 10 — not exercised by the smoke test),
remaining `ref_count == 1` sites in less common code paths
(class 2). Each is small and bounded; queue them up as the
thermometer surfaces them.

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

- Hooked the remaining single-target write chokepoints:
  `JS_DeleteProperty`, `JS_SetPrototypeInternal`, and `OP_put_field`'s
  fast path. All three now route a base target through
  `js_object_for_write` before the mutation. `JS_DefineGlobalVar`'s
  `var`-on-global-obj branch (line 11811) and `JS_DefineGlobalFunction`
  still write to base directly — open follow-up.

  Bench extended to A/B/C cases:
  - A (globalThis.blah set):     ~3.8 µs/iter
  - B (override base GREETING):  ~3.5 µs/iter
  - C (delete base GREETING):    ~3.5 µs/iter
  - reset alone (floor):         8 ns/iter

  All three pass the leak detector across 50000 iterations under both
  Release and Debug+ASan.

- **Resolved: lazy-init wrote to base on first prototype-method access.**
  Tracked back to `JS_AutoInitProperty` calling `js_shape_prepare_update`
  on a base prototype: the path cloned the shape (request arena) and
  wrote `p->shape = clone` into the base JSObject. After the first
  request, `Array.prototype->shape` (and others) pointed at request-
  arena memory; reset wiped that memory; subsequent reads found garbage.

  **Fix: pre-force all autoinit properties at snapshot init**
  (`JS_ForceAllAutoinit`, called by `JS_FreezeRuntime` *before* the
  dual arena flips). Walks `rt->gc_obj_list`, finds every JSObject
  property with `JS_PROP_AUTOINIT`, runs the autoinit function. Iterates
  to fixpoint because instantiating one autoinit (e.g. a function value)
  registers new objects with their own autoinit properties. After this,
  no `JS_AutoInitProperty` ever fires post-freeze, no
  `js_shape_prepare_update` runs on base, no base writes from prototype-
  method reads.

  This is the right shape for our model: lazy init is an antipattern
  for a snapshot-based runtime. Anything that *can* be init at
  snapshot-build time *should* be — the cost amortizes across all
  workers via the shared mmap, and per-request work (the metric we
  optimize) goes down. Lazy init also creates latency variance: the
  first request pays a cost subsequent ones don't, bad for tail
  percentiles. Generalize: revisit any other lazy-init path that fires
  during request execution (`js_module_ns_autoinit`,
  `js_bytecode_autoinit`, etc.) and pre-force at freeze.

  **Bench D restored** — `[1,2,3].reduce((a,b)=>a+b,0)` and similar
  workloads now stable across 1000+ resets. Snapshot grew by ~150 KB
  (all the prototype methods now eagerly allocated), reset cost
  unchanged at 8 ns/iter.

- **test262 walker (`arena-test262.c`)** built as the breadth check
  for the inviolate-base invariant. Walks the test262 spec corpus,
  runs each test under the thermometer in arena mode, asserts zero
  base bytes dirtied per file. Started at "no signal beyond a 38-test
  kitchen-sink stress harness", landed at:

  | tree                | tests | base-clean |
  |---------------------|------:|-----------:|
  | test262/built-ins   | 21570 | **21570**  |
  | test262/language    | 18770 | **18770**  |
  | test262/annexB      |  1003 |  **1003**  |
  | test262/staging     |  1393 |  **1393**  |
  | test262/intl402     |  3284 |  **3284**  |
  | **total**           | 46020 | **46020**  |

  **46 020 / 46 020 evaluated tests across the entire test262 corpus
  leave zero base bytes touched.**

  Classes of base mutation this exercise surfaced and fixed (incremental):
  - `JS_SetGlobalVar` fast path overwrote base `global_obj` prop slot
    (e.g. reassigning `Array`); the surviving request-arena pointer
    crashed the next request. Routed through `js_object_for_write`.
  - `Object.setPrototypeOf(<primitive>, ...)` decoded the primitive as
    a JSObject and ran the arena guard before the spec early-return.
  - `WeakMap`/`WeakSet`/`WeakRef`/`FinalizationRegistry` over base
    targets wrote `target->first_weak_ref` for collection-cleanup
    bookkeeping. Base targets are immortal in arena mode → skip insert
    and the symmetric delete/finalizer paths.
  - `JS_SetPrototypeInternal` flipped `proto->is_prototype = true` on
    rare base prototypes that `JS_MarkAllPrototypes` didn't reach at
    freeze. Skipped (the only consumer is the
    Array.prototype/Object.prototype identity check, both pre-marked).
  - Same function: shadow swap broke identity comparisons (Object.prototype
    immutability + cycle detection). Keep `p_base` for identity, use `p`
    only for writes.
  - `Math.random` updated `ctx->random_state` (same-value write but page-
    faulted). Relocated to `JSRequestState.random_state_req`.
  - `JS_PreventExtensions` wrote `p->extensible = false` directly on
    base targets. Routed through the shadow.
  - `JS_MarkAllPrototypes` added at freeze to pre-mark every base
    prototype's `is_prototype` flag (path-copy, not bulk relocation —
    snapshot stays maximal).
  - `compact_properties` mutated `gc_obj_list` neighbours when the old
    shape was a base shape. Gated to skip list ops in arena mode.
  - `error_back_trace` relocated from `ctx->` to JSRequestState — fixed
    the try/catch/Array.from base writes.
  - `Reflect.set(target, key, val, receiver)` with a base receiver
    routed `JS_CreateProperty(p=base, ...)` and `add_property` wrote
    `p->shape = new_sh` directly into base. Shadow `p` at
    `JS_CreateProperty` entry so the slow path's slow path also
    respects the inviolate base.
  - `JS_GetOwnPropertyInternal2` (the central own-property reader, used
    by `hasOwnProperty`, `Object.keys`, `getOwnPropertyDescriptor`,
    etc.) read `p->shape` without consulting the per-request shadow.
    A base object that had been shadowed-and-mutated this request
    appeared unchanged to those readers. Route `p` through
    `js_object_active` at entry.

## Future direction: request-allocator hybrid (back-pocket)

Not built. Documented so that if the request bump arena's allocation
ceiling becomes a problem on release, the fix is already designed and
we don't rediscover it under pressure.

### The property that bites

The request bump allocator never reclaims mid-request — `js_free` is a
no-op, memory comes back only at `JS_ResetRequestArena`. Consequence:

> a request's arena consumption is its **cumulative** allocation, not
> its **peak live set**.

A handler whose live set is tiny at every instant can still exhaust
the arena if it *churns* — `JSON.parse` a large body, `.map`/`.filter`
through intermediate arrays, build-and-discard strings in a loop. A
GC'd runtime reclaims those; arenajs holds every byte until reset. At
~12M ops/sec a 10 ms handler touches ~100k+ allocations; an
allocation-heavy transform can run to tens of MB of transient garbage.
This is the documented arena trade-off, not a bug — but it is a real
ceiling, and it surfaces as QJS-ng's OOM-during-error-construction
fallback (`current_exception = JS_NULL`, see commit fixing the
"exception: null" diagnostics).

### The linchpin insight

Address determinism + CoW shadowing → fast snapshot is a property the
**base** needs (snapshot/restore is base-only; restore is memcpy/CoW
to identical virtual addresses with no pointer relocation). Request
allocations are never snapshotted — discarded at request end. Replay
determinism comes from the I/O tapes, not memory addresses; JS
semantics never expose addresses (object identity and Map/Set order
are insertion-ordered regardless of allocator). So **request-time
address determinism is incidental, not load-bearing.** The
`JS_FreezeRuntime` boundary cleanly separates the two concerns and is
a natural allocator-switch point. Nothing about the snapshot
machinery, tapes, or replay determinism is affected by changing the
request-side allocator.

### The hybrid design

Generational shape, one-way switch, per-request:

- **Bump region = fast path / young gen.** Request allocations bump
  exactly as today (~3 instructions). Handlers that fit pay *zero*
  free-list cost — today's performance preserved exactly, no
  regression for the common case.
- **Slab + free-list = overflow / old gen.** When the bump pointer
  would exceed the region, the request flips into slab mode instead of
  OOMing. From there `js_free` returns slots to a segregated/TLSF-style
  free-list inside a fixed slab, so refcount-reclaimed memory is
  reused. Churn after the switch is bounded by peak live set, not
  cumulative.

Net effect: **OOM stops being a failure and becomes graceful
degradation** — a handler that exceeds the bump region runs slower
(free-list alloc) but keeps going and recycles its churn, instead of
the request erroring out.

End-of-request reset stays O(1): reset the bump pointer **and** reinit
the slab header — walk nothing, same as today.

### Subtleties to respect when building it

1. **One-way, separate regions.** Objects already in the bump region
   can't be retroactively reclaimed (no headers/sizes to build a
   free-list over). Request memory is `[bump][slab]`; reclamation only
   benefits post-switch allocations. Don't try to convert the bump
   region in place.
2. **`free()` origin test.** The allocator hook decides bump-region
   (no-op) vs slab-region (return to free-list) by a pointer-range
   check `ptr ∈ [slab_base, slab_end)` — one comparison. `realloc` of
   a bump object after the switch → fresh slab alloc + copy; old bump
   bytes leak until reset (fine).
3. **Split point is a tuning knob, counterintuitive direction.** A
   *smaller* bump region makes churning handlers switch into
   reclaiming mode sooner (more headroom) but pushes well-behaved
   handlers onto the slower path earlier. Likely: bump sized to cover
   the p50 handler entirely, slab sized to absorb the heavy tail.
4. **Cycle collector stays off.** QJS reclaims cycles via mark-sweep,
   not refcount; running it mid-request reintroduces the
   pause/nondeterminism the arena design avoids. Leave it off — cyclic
   garbage still accumulates until reset, but that's exactly today's
   behavior, so strictly better, never a regression. Acyclic churn
   (the overwhelming majority) is reclaimed.
5. **Hot-path cost.** The slab allocator only taxes requests that
   overflow the bump region; the bump fast path is untouched, so
   well-behaved handlers see zero slowdown. Measure the slab path's
   per-alloc cost against the existing benches before committing.

### Convergence with the snapshot tooling

`snapshot_here`'s `emit_state` churn is post-freeze, request-time,
acyclic allocation. A refcount-reclaiming request allocator reclaims
it automatically (`emit_state` already `JS_FreeValue`s everything). So
this hybrid **subsumes** the cursor pass-2 OOM fix — the
byte-serialized-`emit_state` rewrite becomes an optional optimization
rather than a necessity. One architectural change collapses two
problems, which is a strong signal the direction is right.

### Trigger

Deploy this if, on release, real handlers hit the request-arena
ceiling (watch for the `tag=NULL` OOM diagnostic in production logs).
Until then it stays designed-but-unbuilt — the bump-only allocator is
simpler and faster for handlers that fit, which is the expected
common case.
