# arenajs

A fork of [quickjs-ng](https://github.com/quickjs-ng/quickjs) tuned for
**one-shot, per-request JS execution on a server**. The runtime is
initialized once into a frozen "base" snapshot; each request runs in a
"request" arena that resets in **a single bump-cursor write
(~9 ns)**. No GC, no `free` calls, no memcpy of the snapshot.

This README is the entry point for embedders. If you're here from the
**rove** project to swap out the existing memcpy-restore vendor, jump
to [Migrating from a memcpy-restore vendor](#migrating-from-a-memcpy-restore-vendor).

## What's different vs upstream quickjs-ng

| | upstream | arenajs |
|---|---|---|
| Allocator | system malloc + GC | dual bump arena |
| Per-request reset cost | N/A (long-lived runtimes) | **9 ns** (one store) |
| Free / GC | refcount + cycle collector | both no-op; arena is the GC |
| Determinism | wall-clock random/time | seeded; `Math.random()`, `performance.timeOrigin`, `Date.now()` are deterministic by default |
| Snapshot model | external (memcpy + bitmap relocation) | **internal** (cursor reset over a frozen base) |
| Spec coverage | full test262 | full test262, **verified base-clean** (see below) |

The headline trade-off: **the snapshot's base bytes are inviolate**.
No JS code (not even pathological) is allowed to mutate snapshot
memory. Anywhere upstream would write through into a base JSObject /
JSShape / atom, arenajs either pre-forces the work at freeze, gates
the write, or shadows the target into the request arena (path-copy
style). The verification this works is in
[`arena-test262.c`](arena-test262.c): it runs the entire test262
corpus under a page-protected base + SIGSEGV thermometer.

**Current state:** `46,020 / 46,020` test262 tests across all five
trees (built-ins, language, annexB, staging, intl402) leave zero
base bytes touched and do not crash. See `make test-arena`.

## API at a glance

The whole new API surface is in [`qjs-arena.h`](qjs-arena.h). The
core lifecycle is three calls:

```c
#include "quickjs.h"
#include "qjs-arena.h"

/* 1. Create the runtime + context, do all init.
 *    base_size and request_size are fixed mmap'd buffers; pass 0
 *    for the 16 MiB default. Allocations beyond capacity return NULL
 *    and propagate as JS OOM. */
JSRuntime *rt = JS_NewRuntimeArena(/*base*/ 64 * 1024 * 1024,
                                   /*request*/ 16 * 1024 * 1024);
JSContext *ctx = JS_NewContext(rt);

/* ... eval prelude scripts, register native functions, build any
 * snapshot-time globals — all of this lands in the BASE arena ... */

/* 2. Freeze: flips allocation mode to the request arena, pre-forces
 *    every lazy initializer, pre-marks every base prototype, then
 *    relocates the per-request mutable runtime state. After this
 *    point, NO further base bytes are written by JS execution. */
JS_FreezeRuntime(rt);

/* 3. Per-request loop. */
for (;;) {
    JSValue v = JS_Eval(ctx, request_src, len, "<req>", JS_EVAL_TYPE_GLOBAL);
    /* ... use v, marshal a response ... */
    JS_FreeValue(ctx, v);

    /* Reset: one store rewinds the request-arena cursor and re-inits
     * the per-request state struct. ~9 ns. The base snapshot
     * (prototypes, atoms, bytecode, ICs) is unchanged and shared. */
    JS_ResetRequestArena(rt);
}
```

There is no `JS_FreeRuntime` / `JS_FreeContext` step in arena mode.
Teardown is `js_dual_arena_free(JS_GetDualArena(rt))`, which `munmap`s
the buffers. The runtime is intentionally non-disposable as a
running entity — it's an immortal piece of process state.

### Determinism helpers

Used at request boundaries to seed per-request behavior without
mutating base:

```c
/* Per-request RNG state. State is reset to 0 (Math.random() == 0)
 * on every JS_ResetRequestArena. Call before evaluating the request
 * if you want non-zero output. */
JS_SetRandomSeed(ctx, request_seed);

/* Per-request time origin. performance.timeOrigin is a getter
 * reading whatever you set here; performance.now() is relative. */
JS_SetTimeOrigin(ctx, wall_clock_ms);
```

Both are stored in `JSRequestState` (request arena) so the writes
don't dirty base.

### Verification (the thermometer)

`qjs-arena.h` exposes an mprotect-based "thermometer" used by the
test harnesses to assert zero base writes per request. Embedders
generally don't need it in production, but it's the right tool when
debugging a regression:

```c
JS_FreezeRuntime(rt);
js_arena_thermometer_enable();
for (each request) {
    js_arena_thermometer_reset();
    /* ... eval ... */
    if (js_arena_thermometer_pages() != 0) abort();  /* base was written */
    JS_ResetRequestArena(rt);
}
```

There's also `js_arena_thermometer_changed_bytes()` for byte-level
attribution and `js_arena_thermometer_trace_range(lo, hi)` to print
a backtrace at every fault inside a chosen offset window — used to
identify which call paths still mutate base when adding a new feature.

## Constraints

These follow from the model and aren't going to change:

- **One arena-backed runtime per process.** A process-global pointer
  range (`js_arena_base_lo` / `js_arena_base_hi`) is set by
  `JS_FreezeRuntime` and used by the refcount chokepoints to skip
  base targets. A second `JS_NewRuntimeArena` in the same process
  would overwrite this and corrupt the first.
- **Single context per runtime.** A few JSRequestState fields
  (`error_back_trace_req`, `random_state_req`) are per-runtime, not
  per-context. Multi-context support would require those to become
  per-context like `shadow_map`.
- **No `JS_FreeRuntime` / `JS_FreeContext` after freeze.** GC is
  suppressed; refcounts on base objects are no-ops. Teardown is
  `js_dual_arena_free`.
- **Fixed buffers.** `base_size` and `request_size` are mmap'd at
  creation and never grow. Sizing them is up to the embedder;
  smoke/stress harnesses use 16 MiB / 16 MiB and 64 MiB / 16 MiB.
- **Weak references on base targets are no-ops.** WeakMap/WeakSet/
  WeakRef/FinalizationRegistry skip the cleanup-notification chain
  for base targets because base targets are immortal — the
  collections still work as strong references during the request.

## Building

CMake, same as upstream:

```sh
cmake -B build
cmake --build build -j
```

Targets produced:
- `build/qjs`, `build/qjsc`, `build/api-test`, `build/run-test262`
  (unchanged from upstream)
- `build/arena-smoke` — basic two-request lifecycle smoke test
- `build/arena-stress` — 38 JS workloads (Array methods, Object,
  String, JSON, Map/Set, closures, classes, try/catch, generators,
  destructuring, RegExp, Date, Symbol, Proxy, Promise, defineProperty)
- `build/arena-bench` — per-request reset speed benchmark
- `build/arena-test262` — the test262 walker (requires the test262
  submodule, see below)

### Running the regression sweep

```sh
git submodule update --init --depth=1 test262   # one-time
make test-arena                                  # builds + runs everything
```

`make test-arena` runs all four arena harnesses in order. The
arena-test262 walker runs all five test262 trees; any non-zero
"base dirtied" count is a regression.

### Bench numbers (Release, single thread)

```
A: globalThis.blah set                  3682 ns/iter
B: override base GREETING               3437 ns/iter
C: delete base GREETING                 3445 ns/iter
D: 100x push + reduce                  18541 ns/iter
reset alone (floor)                        9 ns/iter
```

Most of the per-request cost is parse + compile + eval of the source.
The reset itself is the floor (9 ns). Pre-compiled bytecode gets you
closer to the floor.

## Migrating from a memcpy-restore vendor

If you currently vendor quickjs-ng with a custom snapshot/restore
layer (the rove pattern: bump arena + `memcpy` of a frozen image +
bitmap-driven pointer relocation + per-context volatile-slot reseed),
the swap is mostly *deletion*:

1. **Replace your vendored quickjs-ng files** with this repo's
   versions. The C sources you need are:
   ```
   quickjs.c quickjs.h
   qjs-arena.c qjs-arena.h     ← NEW, must add to your build
   libregexp.c libregexp.h libregexp-opcode.h
   libunicode.c libunicode.h libunicode-table.h
   dtoa.c dtoa.h
   cutils.h list.h
   quickjs-atom.h quickjs-c-atomics.h quickjs-opcode.h
   builtin-array-fromasync.h builtin-iterator-zip.h builtin-iterator-zip-keyed.h
   ```
   Add `qjs-arena.c` to your existing C-sources list. No other build
   flag changes — `-D_GNU_SOURCE` and the rest carry over verbatim.

2. **Drop your bump arena malloc-funcs.** `JS_NewRuntimeArena`
   installs `js_dual_arena_malloc_funcs` for you. The base/request
   split lives inside `JSDualArena` (one mmap'd buffer per side).

3. **Drop your snapshot create/restore code.** Replace:
   - `Snapshot.create(init_fn)` → `JS_NewRuntimeArena(...)` + run
     init eagerly + `JS_FreezeRuntime(rt)` once.
   - `Snapshot.restore(target_arena)` → `JS_ResetRequestArena(rt)`.
     No memcpy, no bitmap walk, no pointer relocation — the base
     buffer is shared in place across all requests.

4. **Drop the volatile-slot machinery.** No more two-pass diff to
   detect non-deterministic JSContext fields. Determinism is
   built-in: `Math.random()` and `performance.timeOrigin` start at
   0 every request; seed via `JS_SetRandomSeed` /
   `JS_SetTimeOrigin` if needed.

5. **Drop any "stable shape hash" or determinism patch you'd
   applied** to upstream quickjs-ng. Equivalent fixes are already
   in the fork (and verified by `make test-arena`).

6. **Drop `JS_FreeRuntime` / `JS_FreeContext` from your hot path.**
   Lifetime ends with `js_dual_arena_free` at process exit (or
   never, if the runtime is process-singleton).

After the swap, your per-request loop becomes:

```c
/* startup */
rt  = JS_NewRuntimeArena(BASE_SZ, REQ_SZ);
ctx = JS_NewContext(rt);
eval_prelude(ctx);
JS_FreezeRuntime(rt);

/* hot loop */
for each incoming request {
    JS_SetRandomSeed(ctx, random_u64());      // optional
    JS_SetTimeOrigin(ctx, wall_clock_ms());   // optional
    handle(ctx, request_body);                // calls JS_Eval, etc.
    JS_ResetRequestArena(rt);                 // 9 ns
}
```

A complete worked example is [`arena-smoke.c`](arena-smoke.c) — two
requests with a prelude, the thermometer wired up, and the
determinism helpers exercised.

## License

Same as upstream quickjs-ng: MIT. See [`LICENSE`](LICENSE).
