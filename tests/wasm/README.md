# WASM smoke tests

End-to-end checks for the browser-targeted `qjs_arena_wasm` build —
replay-mode tape bindings, module loader, trace emitter, stack
walker, RTAP wire-format compatibility.

## Build the WASM target

```sh
source ~/src/emsdk/emsdk_env.sh   # or equivalent for your emsdk install
mkdir -p build-wasm
cd build-wasm
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake make qjs_arena_wasm
```

That produces `build-wasm/qjs_arena_wasm.{js,wasm}`. The tests import
the JS glue from there relative to their location.

## Run the tests

From the repo root, with a node that supports ES modules (the one
bundled with emsdk works fine — `node tests/wasm/<name>.mjs`):

| File              | What it covers                                                    |
|-------------------|-------------------------------------------------------------------|
| `smoke.mjs`       | The WASM module loads and evaluates JS at all                    |
| `replay-smoke.mjs`| Native bindings for kv / date / math_random / crypto channels    |
| `module-smoke.mjs`| Module loader consumes from the module tape                      |
| `date-smoke.mjs`  | `new Date()` no-arg consumes from the date tape                  |
| `trace-smoke.mjs` | Trace emitter — scan + drill modes, stop sentinel                |
| `state-smoke.mjs` | Stack walker — args, locals, captured locals, closure vars       |
| `wire-smoke.mjs`  | RTAP bytes → parser → Module.tapes → handler — full round-trip   |
| `cursor-smoke.mjs`| `CursorEngine` — scan-index cache, scan/line anchors, paging     |
| `snapshot-here-smoke.mjs` | `arena_snapshot_here()` — snapshot mid-run without stop  |

`rtap.mjs` is the parser/serializer module the wire test depends on.
It mirrors `src/tape/root.zig`'s encoding rule-for-rule and is the
intended lift target for rove's web/ tree when the new replay UI
needs a shared parser.

`cursor.mjs` is the host-side replay cursor module (design notes in
`REPLAY_CURSOR_API.md` at the repo root). Same lift trajectory as
`rtap.mjs` — temporary home here, eventual destination is rove's
web/ tree.
