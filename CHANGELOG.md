# Changelog

All notable changes to the arenajs **embedder contract** are recorded
here. Format follows [Keep a Changelog](https://keepachangelog.com/);
versioning is [Semantic Versioning](https://semver.org/) applied to
the contract, not to the fork as a whole.

**Base:** quickjs-ng 0.16.0. arenajs versions independently; the
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

### Upstream sync — the rest of 0.16.0 (stage 2c)

The remaining 59 commits, `9de2921..v0.16.0`. Four conflicts in
`quickjs.c`, none of them the interesting part.

Two were pure arenajs determinism additions where upstream's side of the
conflict was empty; they were resolved programmatically with an assertion
that upstream's side really is empty, so 2b's "taking ours silently
deletes upstream's line" failure cannot recur unnoticed. The other two
needed real merging: `js_parse_error` took upstream's column-derivation
(`65e766e`) over our per-request exception access, and `js_new_module_def`
kept our arena-gated list linking while taking upstream's new
`m->attributes = JS_UNDEFINED` — dropping that would have left a JSValue
uninitialised and later freed.

**The problems were all in code that merged cleanly.**

`rt->parent_promise` — upstream's new `JS_PromiseThen` writes a field this
fork moved to `rt->req->parent_promise`. New code in a new function, so no
conflict; only the compiler caught it, and only because the field no
longer exists on `JSRuntime`.

That prompted an audit for the silent version of the same hazard: a field
we *shadow* rather than remove still exists, so new upstream code using it
compiles and quietly bypasses the shadow. Comparing direct-use counts
against the pre-merge tree found `ctx->random_state` going from 3 uses to
7. **`807f271` seeds the Map/Set key hash from the wall clock at context
init** — `ctx->random_state = js__gettimeofday_us()`, with `hash_seed`
derived from it before `js_random_init()` zeroes the state again. Wall
clock reads in context init went from 0 to 1, which is exactly what
`55aff69` exists to prevent. `Math.random` was unaffected; `hash_seed` was
not. Now pinned for arena runtimes, with upstream's randomised seed kept
for vanilla ones where it guards against hash-collision DoS (upstream
issue #205). An arena runtime takes the collision risk knowingly: request
CPU is bounded by the interrupt handler, and a reproducible snapshot is
the product.

### Fixed

**`SetterThatIgnoresPrototypeProperties` recursed until the stack gave
out, on any base-resident Iterator prototype.** Upstream's new setters
(`161db12`, `93d3f7d`) guard with an identity check against a base
pointer:

```c
if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_ITERATOR]))
    return JS_ThrowTypeError(ctx, "Cannot assign to read only property");
```

A request reaches that guard holding the *shadow* of the home object —
`JS_SetPropertyInternal` swaps `this_obj` to the shadow in lockstep with
the receiver — so the comparison misses, the setter falls through to
`JS_SetProperty(this_val, ...)`, and re-enters itself. `Iterator.prototype
.constructor = ""` from a request was enough.

Same class as the `setPrototypeOf` identity bug, and the remedy was
already in the tree: `js_object_base_identity()`, whose comment claimed
its only caller was `JS_SetPrototypeInternal`. Both new setters now route
through it.

Worth noting what did *not* find this. It is not a merge conflict. ASan is
silent, because stack exhaustion is not a heap error. Calling the setter
explicitly (`set.call(P, "")`) does not reproduce it — only plain
assignment goes through the receiver-swapping path. It surfaced because
this merge bumps the test262 submodule (`d5e73fc` -> `5ef1e57`) and the
newer corpus carries `built-ins/Iterator/prototype/*/weird-setter.js`.

**Any upstream code comparing `this` against `ctx->class_proto[...]` is a
shadow-identity hazard, and it will never appear as a conflict.**

### Upstream sync — the allocator commit (stage 2b)

One commit, upstream `9de2921` "Add arena allocator", taken on its own
because it touches 53 of the 181 `quickjs.c` functions arenajs patches
where every other commit in the 0.16.0 range touches one to four.

It does two separable-in-principle things, and only one of them is
usable here.

**The refcount relocation, adopted.** `JSRefCountHeader` is deleted
outright and `JSGCObjectHeader` keeps only its list link; `ref_count`,
`gc_obj_type` and `mark` move into an 8-byte `JSMallocBlockHeader`
preceding every allocation, reached through `JS_REF_COUNT()` /
`JS_GC_TYPE()` / `JS_GC_MARK()`. This is not optional and not really
"upstream's allocator" — it is where the engine's refcount now lives,
including the base-object refcounts `arena_rc_inc`/`arena_rc_dec` guard.
The model is untouched: `js_arena_ptr_is_base()` is an address-range
test and the block header sits inside the same allocation, so a base
object's refcount is still base memory under the same PROT_READ
hardening. Only the address written moves.

The port came to roughly 45 sites rather than the dozen the chokepoints
suggested, because `9de2921` also relocates `JSShape`'s properties out
of the struct (`sh->prop[i]` becomes `get_shape_prop(sh)[i]`).

**The small-block pool, disabled for arena runtimes.** Its bookkeeping —
`JSArena.first_free_block`, `.n_used_blocks`, the `free_arena_list`
links — is per-runtime and outlives the freeze boundary. Any size class
whose newest arena is partially full at `JS_FreezeRuntime` leaves that
`JSArena`, allocated in base, on `free_arena_list`; the first
post-freeze request allocation in that class writes it. Letting arena
runtimes pool measures as `arena-smoke` reporting 30 dirtied base pages
and four other harnesses taking SIGSEGV.

Gated at **runtime** rather than with upstream's compile-time
`JS_ARENA_LARGE_BLOCKS_ONLY`, because `js_arena_malloc` has `rt` in
scope. Vanilla runtimes — the `qjs` CLI, plain embedders, the vanilla
half of `arena-coexist` — keep the pool and its ~18% geomean; arena
runtimes always take the large path. Mixing is safe in both directions
because `js_arena_free` is self-describing: `arena_malloc_large` stamps
`block_idx = JS_ARENA_FREE_NIL`. An arena runtime forfeits nothing it
wanted — pooling recycles small blocks across a long-lived heap, which
is the workload a one-shot per-request VM structurally does not have.

**One bug worth recording, because it was ours and not upstream's.**
The conflict resolution took the HEAD side of all 36 conflicts, which is
correct where both sides carry content but silently *deletes* upstream's
line where our side is empty. That happened once, in `js_realloc_rt`,
and left four arena fast paths calling `rt->mf.js_*` directly with a
user pointer instead of going through the `js_arena_*` wrappers that
reach back to the block header — so the backing allocator read eight
bytes off the front of its own chunk. It presented as a SEGV in
`mspace_realloc`. Treating it as a class rather than a site turned up
all four, plus `old_size` taking usable-size from the raw pointer. A
fifth site lived inside an `assert()` and was therefore invisible until
the Debug/ASan build.

### Upstream sync — 0.15.1 → 0.16.0, pre-allocator (stage 2a)

18 commits, `v0.15.1..377a25e`. The 0.16.0 range is deliberately **not**
taken as one merge: upstream `9de2921` ("Add arena allocator") touches 53
of the 181 `quickjs.c` functions arenajs patches, where every other commit
in the range touches one to four. Cutting immediately before it keeps that
port reviewable on its own. The range is linear, so the cut is clean.

Two conflicts.

`9b57175` (Error.prototype.stack accessor proposal) restructures the tail
of `build_backtrace` into a three-way branch — `captureStackTrace` target,
genuine Error, DOMException-and-the-like. Took upstream's structure whole
and substituted our per-request `js_error_back_trace_*` accessors for
`ctx->error_back_trace`. The new middle branch writes `p->u.object_data`
directly, which bypasses the shadow, so it is annotated: `build_backtrace`
only ever runs on a freshly constructed error, never a base one. The
`Error.prototype.stack` *setter* was checked separately and is safe — it
routes through `JS_DefinePropertyValue` / `JS_SetPropertyInternal2`, both
shadow-covered.

`249bb27` (Enforce immutability on TypedArray write paths) collided with
our own `336ad63`, and upstream won on the merits. We had added an
immutability check to `js_typed_array_reverse` because upstream lacked one
("every other in-place typed-array mutator checks this and it does not").
Upstream has now added exactly that check and placed it *before* the
`len > 0` guard, so it also throws for a zero-length view. Ours was
deleted as redundant.

That pair is the quiet win of this stage. Our base-ArrayBuffer protection
does not implement its own enforcement — `336ad63` sets upstream's
`abuf->immutable` flag on every base buffer at freeze and relies on
`typed_array_is_immutable(p)` to reject writes. `249bb27` keeps that exact
predicate and applies it in more places (clearing 18 test262 expected
failures), and `eb52863` fixes the ArrayBuffer immutable-method semantics
around it. The base guarantee got stronger without us writing anything.

Also inherited: `5f2fb55` ports Bellard's register-based regexp engine
(+1657/-745 in `libregexp.c`, a file this fork has never touched, so it
merged untouched) — it brings the `v` flag, regexp modifiers, duplicate
named capture groups and properties-of-strings, and it removes
`tests/bug1221.js`, whose bug the new engine does not have. `377a25e`
inlines mixed int/float arithmetic and fast-array reads in the interpreter
(+101/-0); its six new `u.array.` sites are reads, so they cannot violate
base, but they are a class-13 audit surface if a write path is ever added
alongside them.

The expected-failure baseline drops 96 -> 74; the 22 are exactly what
`249bb27` (18) and `eb52863` (4) fix. Two entries report a *changed*
message rather than a changed verdict — `subarray/byteoffset-with-detached-buffer.js`
now fails at the `makePassthrough` sub-case instead of `makeArrayBuffer`,
i.e. it gets further than the recorded text. Building vanilla upstream at
`377a25e` reproduces the identical drift, so this is upstream's own
`test262_errors.txt` being briefly stale mid-release, not a divergence
here; `83bd0ab`, just past our cut, fixes the bug and drops the entry.

MINOR: no `arena_*` export changes, no wire-format changes. New JS-visible
surface — `Error.prototype.stack` is now a prototype accessor rather than
an own data property, the regexp engine gains `v`-flag and modifier
support, and `4fb9b0c` implements nonextensible-applies-to-private.
`05f0bde` adds the `JS_FreeValues`/`JS_FreeAtoms` vararg macros to
`quickjs.h`.

### Upstream sync — quickjs-ng 0.14.0 → 0.15.1

First of a staged catch-up with upstream; 28 commits. Merged rather
than rebased, and cut at a release tag rather than at `master`, so each
landing has a bounded diff and its own run of the arena harnesses.
Three conflicts, all in `build_backtrace`, all the same shape: upstream
`e1c1e41` duplicates `error_val` into a local to fix a use-after-free
when a `DynBuf` OOM frees the current exception, while our side had
de-globalised the reentrancy flag onto `rt->req` and moved
`ctx->error_back_trace` behind accessors. Resolution takes upstream's
lifetime fix and keeps the per-request state; every `goto done` path
reaches the new `JS_FreeValue`.

MINOR: no `arena_*` export changes, no wire-format changes. New
**JS-visible surface** an embedded program can now use — explicit
resource management (`using`, `await using`, `DisposableStack`,
`AsyncDisposableStack`) — plus upstream additions to `quickjs.h`
(`JS_NewUint64`, the `JS_FreeValues`/`JS_FreeAtoms` vararg macros,
`JS_ABORT_ON_LEAKS`). `__JS_NewShortBigInt` moved out of `quickjs.h`
into `quickjs.c`; it is `__`-prefixed internal API and no embedder in
this tree or in rove referenced it.

Inherited fixes that matter here rather than generally: the
`build_backtrace` use-after-free above, a fast-array expansion overflow
(`a653771`) and the completion of the fast-array delete use-after-free
fix (`da49a37`) — both in the same element storage our base fast-array
copy-on-write shadows — and two source-position corrections the trace
UI reads, `source_loc` around `iterator_close` so a `for`/`of`
`return()` frame reports the right line (`bcac5c2`) and column
reporting for invalid number literals (`19fa597`).

### Fixed

**Base-resident `DisposableStack` and `AsyncDisposableStack` are now
copy-on-write.** The two classes arrive with their state — a `disposed`
flag and a growable `resources[]` array — hanging off `u.opaque`, so
the shallow struct copy in `js_clone_jsobject_for_write` left a
shadowed stack pointing at the snapshot's own struct: `use()`,
`defer()` and `adopt()` appended to the *snapshot's* resource list and
`dispose()` flipped the *snapshot's* flag, so request 2 inherited
request 1's registered resources and could find the stack already
disposed. Growing `resources[]` past capacity is the worse case — the
`js_realloc` relocates a base table into the request arena, which is
rove#735's failure mode. Closed the same way as the iterator cursors:
the state struct and its array are copied into the request arena on
shadow creation, and the four mutating entry points plus the dispose
path resolve through `JS_GetOpaqueForWrite`. The `disposed` getter
stays a plain read, so inspecting a base stack still costs no shadow.

`arena-baseclass` found this the turn the merge landed, by design — it
renumbered against the class enum, reported the two new ids as
unclassified, and then reported the base writes once they were given
mutation ops. The matrix is now 34 clean / 4 refused at freeze / 0
known gaps.

## [0.4.0] - 2026-08-21

The **base-isolation release**. A crash filed against the embedder
(rove#735) turned out to be one instance of a much larger hole: the
shadow mechanism protected a base object's *named properties* and
nothing else, so every other kind of per-object mutable state — array
elements, buffer bytes, closure cells, iterator cursors, a Date's time
value — was written straight into the snapshot and carried into the
next request. On a multi-tenant worker that is a cross-request channel,
and in two cases a dangling pointer. Six families closed by
copy-on-write or immutability, four kinds of state refused at freeze
because isolating them has no non-arbitrary meaning, and a
class-table-driven test that fails when a new class is added
unclassified so this cannot silently regrow.

MINOR: two new exports (`JS_ScanSnapshotHazards`,
`JS_MarkAllBaseArrayBuffersImmutable`); every existing export keeps its
name, signature and return codes. The ⚠ items change *when* an
already-invalid program aborts, not any working program's behaviour —
`make test262` is byte-identical to the previous release throughout.

**⚠ Contract — `JS_FreezeRuntime` now refuses a snapshot holding state
that cannot be isolated per request**, and aborts with a report naming
each offender and *where it is reachable*. New public
`JS_ScanSnapshotHazards(rt, FILE*)` performs the same scan without
aborting, so an embedder can check before freezing and fail its own
way — which matters when the same prelude is frozen into several
contexts (a worker, an offline sim, a browser replay arena), since
otherwise one mistake has to be diagnosed once per context.

Refused: **pending promises, generators, async generators,
FinalizationRegistry**. Allowed, deliberately: a **settled** promise
with no pending reactions — `Promise.resolve(x)` as a memoised value is
reasonable in a snapshot, and its only base write is a debug-only flag.

The pending-promise case is not a tidiness problem. After the first
`.then()`, the base promise's reaction-list head points *into the
request arena*, and the reset recycles it. Later requests report zero
changed bytes — which reads as benign and is not: the bump allocator is
deterministic, so it hands back the same address and the pointer is
rewritten identically. It is a latent dangling pointer wearing a clean
diff, so refusing removes a crash rather than tidying a visibility
problem.

For the others the objection is that copy-on-write has no
non-arbitrary meaning. A generator half-consumed at freeze would
restart mid-body every request — defensible, arbitrary, and not
something anyone writing `function*` in a prelude is reasoning about.
Copying a promise's reaction list would make a resolution in request N
invisible to N+1: correct isolation that contradicts intent, which is
the worst kind of thing to debug from a support ticket.
FinalizationRegistry entries carry weak-ref bookkeeping that lives in
base, and its callbacks can never fire in an arena runtime anyway.

The report gives a path, not just a class, because an embedder hitting
this is looking at twenty prelude files with no line number and the
object has no creation site:

```
arenajs: the snapshot contains state that cannot be isolated per request:
  pending Promise        at globalThis.platform.warmup
                         resolve it before freezing, or create it per request
  Generator              at globalThis.tasks.queue[0]
  FinalizationRegistry   at globalThis["weird-key"].reg
  AsyncGenerator         (not reachable from the globals — held by a closure)
```

Cheap when clean: a class-id check per object, with the graph walk only
running once something has been found, and its scratch sized to the
actual object count.

**Fixed: base iterators are copy-on-write.** An iterator keeps its
cursor in a heap struct hanging off `u`, and the shadow's shallow copy
of that union left the pointer aiming at the snapshot's struct — so
`next()` on a base-resident iterator advanced the *snapshot's* cursor
and the following request resumed where the previous one stopped.
Request 2 got element 2. Six classes: `Map Iterator`, `Set Iterator`,
`Array Iterator`, `String Iterator`, `Iterator Helper`, `Iterator
Concat`.

Four distinct state layouts, one uniform mechanism: the shadow gets its
own copy of the state struct, and the `next()` (and `return()`)
implementations resolve through it via a write-aware opaque getter.
Read paths that do not advance a cursor keep the plain getter, so a
plain inspection does not pay for a copy. `Iterator Concat`'s state has
a flexible array member, so its copy is sized from the element count
rather than `sizeof`.

Iterators reach a snapshot far more easily by accident than generators
do, and unlike a generator there is no ambiguity about the right
answer, which is why these are fixed rather than refused.

**Fixed: base closure cells (`JSVarRef`) are copy-on-write.** A
snapshot closure's captured variables live in base, so assigning one
wrote the snapshot and the value carried into the next request — a
cross-request, therefore cross-tenant, channel in the most ordinary
snapshot shape there is, since `globalThis.x = (function(){ let state
= ...; return {...}; })()` is simply how a module is written. A base
counter read 1, 2, 3 across three requests and dirtied two base pages
each time.

Scope, probed rather than reasoned. **Leaks:** a variable captured by
a closure that escapes into the snapshot *and* reassigned after freeze.
**Clean and unchanged:** top-level `let`/`const` in a
`JS_EVAL_TYPE_GLOBAL` script (the global lexical environment is already
shadowed), top-level `var`, a property on a base object, a captured
`const` object whose *property* is mutated, and any captured variable
that is only read. The line is that mutating what a captured variable
*points at* was always fine; rebinding the variable was not.

Copy-on-write keyed on the **cell**, not the closure that reached it —
several closures routinely share one cell, and they must all resolve to
the same shadow or isolation is bought by breaking sharing. Costs
+1.8 ns (+5%) on base closure read+write and nothing on request-local
access.

⚠ **Guidance interaction worth knowing** (surfaced by rove): enclosing
a shim in an IIFE *moves* its top-level bindings from the clean
category into the leaking one, because global lexicals are shadowed and
closure cells were not. "Wrap your shims" — the usual fix for a
name-reachability leak — was therefore incomplete advice on its own and
needed "and keep module scope `const`" beside it. This release closes
the trap either way, but the pairing still matters for anyone on an
older pin.

**Fixed: base `Date` objects are copy-on-write.** A Date's time value
lives in `u.object_data`, inside the `JSObject` itself, so for a
base-resident Date the setters wrote snapshot memory and the mutation
was visible to every later request. `JS_ThisTimeValue` and
`JS_SetThisTimeValue` are the whole surface — every getter reaches the
value through the first and every setter through the second — so both
now resolve to the request-arena shadow. Redirecting only the write
would have left `setTime` updating the shadow while `getTime` still
read base, which is worse than the leak.

The other `u.object_data` classes (`Number`, `String`, `Boolean`,
`Symbol`, `BigInt`) are written once at construction and never
mutated, so they were already clean and are deliberately left alone: a
redirect in the shared `JS_SetObjectData` without chasing all their
read sites would create exactly that read/write split.

**New harness `arena-baseclass`: exhaustive base-object mutation
matrix.** Every base-write bug so far was found by someone thinking of
a case, which neither scales nor converges — the fast-array hole sat
behind a line in `ARENA_PLAN.md` for months, and hand-enumerating the
affected classes afterwards still missed several. This test enumerates
the *engine's* class table instead of a list someone wrote down: each
registered class is either exercised with a live base-resident
instance, or classified with a reason (no mutable state / unreachable
from a JS snapshot / covered by another harness). A class that is
registered but unclassified fails the test, so adding one to the engine
forces someone to consider it. Each id is also pinned to the name
`JS_GetClassName` reports, so an enum reshuffle is caught rather than
silently testing the wrong class.

It found **12 classes** that write base or leak across a reset, none of
which were on the hand-written list: `MappedArguments`, `Date`, six
iterator classes (`Iterator Concat`/`Helper`, `Map`/`Set`/`Array`/
`String Iterator`), `Generator`, `Promise`, `AsyncGenerator` and
`FinalizationRegistry`. A base-resident iterator is the sharpest case —
its cursor leaks, so the second request gets the second element.

Those are recorded as `KNOWN_GAP` entries, which are expected failures
*with teeth*: each is asserted to still be broken, so fixing one turns
the test red until the entry is promoted. The table doubles as the
backlog.

**Fixed: base ArrayBuffers are immutable after freeze.** A snapshot
ArrayBuffer's bytes are base memory, so a request writing through any
view onto it mutated the snapshot and the write was visible to every
later request. On a multi-tenant embedder that is a cross-request
channel — one tenant poisoning a shared base64 decode table for every
other tenant on the thread, which is a live shape in rove's snapshot.

Unlike a fast array these bytes cannot be copy-on-written cheaply: they
belong to the ArrayBuffer, so a copy would have to move every aliasing
view atomically, carry detached/resizable state along, and memcpy the
whole buffer — an unbounded, embedder-controlled per-request cost, i.e.
a capacity cliff rather than a fix. `JS_FreezeRuntime` now marks every
base-resident ArrayBuffer immutable instead, reusing the
immutable-ArrayBuffer support quickjs-ng already has: element stores,
`fill`, `copyWithin`, `sort`, `set`, `transfer`, `resize`, `slice` and
the DataView setters all check it, with upstream's own error. Reads are
unaffected, and `.immutable` reads `true` from JS so a handler can
feature-detect. Copy explicitly — `new Uint8Array(BASE_TA)` — for a
mutable per-request one.

⚠ Note the asymmetry inherited from upstream: the *method* mutators
throw `TypeError: ArrayBuffer is immutable`, but a bare element store
(`ta[0] = x`) is silently refused rather than throwing, because
upstream's `[[Set]]` returns false without propagating to a strict-mode
throw. The write does not land either way.

Two gaps filled while there, both of which also affect non-arena
immutable buffers: `TypedArray.prototype.reverse` and the `Atomics`
read-modify-write ops wrote through without checking immutability.

**Fixed: base fast arrays are now copy-on-write.** The shadow
mechanism covered a base object's *named properties* only —
`js_clone_jsobject_for_write` shallow-copies the `u` union, and the
mutating fast paths took the element pointer straight off the base
`JSObject`. So every in-place array mutation (index store, `sort`,
`reverse`, `fill`, `copyWithin`, `pop`, `shift`, `splice`) wrote into
the snapshot and was visible to **every later request** — a
cross-request data channel — and `push` reallocated
`u.array.u.values` into the request arena and stored that pointer back
into the base object, so the next reset left it dangling and the
following request segfaulted. Same shape as rove#735 but reachable
from ordinary JS with no knife-edge.

The shadow now gets its own copy of the element array, kept in
fast-array form so base arrays keep their compact layout and read
speed, and four chokepoints redirect to it: `JS_SetPropertyValue`,
`js_array_push`, `js_array_pop`, `js_array_reverse`, and
`JS_CopySubArray` (`copyWithin`/`splice`). Read-only fast paths
(`indexOf`, `includes`, `toSorted`, `slice`) deliberately do **not**
redirect — shadowing those would copy the whole element array on a
pure read.

A base array now behaves exactly as a base object always has:
mutations work, they are request-local, and the reset restores the
snapshot. `test262` is unchanged at 97/80017, i.e. no JS semantics
moved; the only observable differences are the leak and the crash
going away. Code that *relied* on the leak — writing a base array in
one request to read it back in the next — will stop seeing it, which
is the point.

New harness `arena-baseobj` asserts both halves independently, because
they fail independently: no base page is dirtied, **and** the next
request sees the snapshot value. Verified red on the unfixed tree (12
failures, including the dangling read returning
`1,2,29,1,[unsupported type]`).

Base typed arrays, ArrayBuffers, `Date`, promises and generators hold
their mutable state elsewhere and are still affected; they are handled
separately.

**Performance: O(1) shadow-table lookup for base objects.** The
base-object shadow map was a linked list probed on every read and write
that touches a base object, so cost scaled with how many base objects a
request had already shadowed. Depth and frequency are independent axes,
so an ordinary handler — mutate part of the prelude, then run a hot
loop — paid the full depth on every iteration. Replaced with an
open-addressed table keyed on the base pointer. Whole-request shapes
improve 3.8×–17.5×; depth-1 is unchanged, and nothing regressed.
`arena-bench` grows three shadow-depth rows that must stay flat in each
other's company. No contract change: no export, signature, return code
or wire format moves, and no observable semantics change.

## [0.3.6] - 2026-08-21

The **inviolate-base repair release**: a base-resident runtime table
could be relocated into the request arena after freeze and then
recycled out from under the runtime, which is the one invariant this
project exists to hold. One fix, one guard against the same mistake
being made through a different door, one sweep harness that can see
the failure, and a header note for an API whose name promised more
than it delivered.

**Fixed: base atom table relocated into the request arena
(rove#735).** `__JS_NewAtom`'s "grow the atom array" branch and
`js_new_shape2`'s "resize the shape hash" branch were not gated on
`rt->is_arena`. Post-freeze they still fired, so `js_realloc_rt`
reallocated the base `rt->atom_array` / `rt->shape_hash` into the
**request** arena and rewrote the pointer in base. The next
`JS_ResetRequestArena` recycled that memory underneath them, after
which any atom lookup — `js_empty_string()` on the first
`JS_NewStringLen(ctx, s, 0)` was the usual first casualty — dereferenced
recycled request bytes and segfaulted.

Both branches are dead weight in arena mode: post-freeze interning
allocates from `rt->req->atom_overlay` and hashed request shapes link
into `rt->req->shape_overlay`, each with its own free list. Gating them
off is the whole fix.

The trigger is a knife-edge: the crash needs `rt->atom_free_index == 0`
at `JS_FreezeRuntime`, i.e. the snapshot's atoms happening to exactly
fill the base array. Adding or removing *one* atom anywhere in the
snapshot flips it, which is why it presented as "one more call site in
one shim and it dies". PATCH: no export, return-code, or wire-format
change.

New harness `arena-atomgrow` (in `make test-arena`) sweeps the
snapshot's atom count one atom at a time and asserts, under the
thermometer, that no post-freeze request dirties base. On the unfixed
tree exactly 2 of 801 sweep points go red — which is why the existing
test262 walk never caught this: the corpus never builds a snapshot that
lands on the boundary.

**⚠ Contract — `JS_NewClass` after `JS_FreezeRuntime` now aborts.** It
grows `rt->class_array` and every `ctx->class_proto` through the same
un-gated `js_realloc_rt`, so post-freeze it relocated them into the
request arena with the identical outcome as above. It was never a
supported call — it corrupted silently, several layers away, in someone
else's request. It now prints the offending class id to stderr and
aborts. Register every class into the snapshot, before the freeze. No
signature or return-code change; callers that already respected the
freeze boundary are unaffected.

**Documented: `js_dual_arena_oom_hit()` is request-arena only.** It
does not and never did report base-mode refusals — a snapshot can
overrun its base arena by any margin with the flag still false, so it
is not a base-exhaustion guard. Base exhaustion surfaces as a thrown
exception from the `JS_Eval` that ran out, so checking `JS_IsException`
on every snapshot-time eval *is* the base-exhaustion check;
`js_dual_arena_base_used()` is the headroom metric. Header comment
only, no behavior change.

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
