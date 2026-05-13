/*
 * arenajs replay-mode JS bindings.
 *
 * Installs native QuickJS functions on the global object that route to
 * host-side replay tapes instead of calling live system services:
 *   - Math.random            ← math_random tape
 *   - Date.now               ← date tape
 *   - crypto.{getRandomValues, randomBytes, randomUUID}  ← crypto_random tape
 *   - kv.{get, set, delete, prefix}                      ← kv tape
 *
 * Mirrors the five channels in rove's src/tape/root.zig (module loader
 * is wired separately via JS_SetModuleLoaderFunc2 in the reactor).
 *
 * Tape state lives in JS (Module.tapes — one array per channel, with
 * per-channel cursors managed inside the EM_JS host imports). All
 * variable-length byte returns from the host are malloc'd in JS via
 * Module._malloc and freed by C after copying into QJS values.
 *
 * Must be called pre-freeze so the binding objects land in base memory
 * and survive per-request resets.
 */
#ifndef QJS_ARENA_REPLAY_BINDINGS_H
#define QJS_ARENA_REPLAY_BINDINGS_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, -1 on registration failure. */
int arena_install_replay_bindings(JSContext *ctx);

/* Install the replay-mode module loader on the runtime. Each `import`
   call consumes the next entry from Module.tapes.module, verifies the
   specifier matches, and pulls source bytes from Module.module_sources
   (keyed by specifier first, falling back to source_hash_hex). The
   default QJS normalizer handles ./ and ../ resolution.

   Must be called pre-freeze. Returns 0 on success. */
int arena_install_replay_module_loader(JSRuntime *rt);

#ifdef __cplusplus
}
#endif

#endif
