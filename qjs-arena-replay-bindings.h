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

#include <stdint.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __EMSCRIPTEN__
/* ── native replay host ────────────────────────────────────────────────
 * In the browser build the replay bindings pull recorded values from
 * Module.tapes via EM_JS imports. A native build has no JS host, so it
 * dispatches each tape read to a C responder the embedder registers here.
 * This is the input side of replay; the trace sink (qjs-arena-trace.h) is
 * the output side. Together they are the full host-hook surface a native
 * replay/simulation driver needs — Date.now and the PRNG are pinned
 * separately via arena_set_date_now() / arena_set_random_seed(), so they
 * need no callback.
 *
 * Every responder returns a tape-outcome code, mirroring the browser host:
 *   0   ok — out-params written
 *   1   tape channel not installed (surfaced to JS as a ReferenceError)
 *   2   tape exhausted (surfaced as an InternalError)
 *   <0  divergence: the handler asked for something the tape didn't record
 *       (wrong op/key/length) — surfaced as an InternalError. Returning a
 *       divergence code is exactly the lever a simulator uses to detect,
 *       or deliberately allow, an off-tape execution.
 *
 * `*out_outcome` is the per-operation result the JS API sees, NOT the tape
 * code above: 0 = ok, 1 = not_found (kv.get → null), 2 = err (throws).
 *
 * Byte-buffer out-params (out_val, out_json, out_src) must be allocated by
 * the responder with malloc(); the engine copies the bytes into a QJS
 * value and then free()s the buffer. out_src (module source) and out_json
 * (kv.prefix rows) MUST be NUL-terminated — allocate len+1 and write a
 * trailing '\0' — because the module/JSON parsers read one sentinel byte
 * past the given length. The inbound key/val/prefix/cursor/spec pointers
 * are borrowed and valid only for the duration of the call.
 *
 * A NULL responder for a channel behaves as "tape not installed" (code 1),
 * so an embedder can register only the channels its workload touches. */
typedef struct {
    int (*kv_get)(const uint8_t *key, int key_len,
                  int *out_outcome, uint8_t **out_val, int *out_val_len,
                  void *user);
    int (*kv_set)(const uint8_t *key, int key_len,
                  const uint8_t *val, int val_len,
                  int *out_outcome, void *user);
    int (*kv_delete)(const uint8_t *key, int key_len,
                     int *out_outcome, void *user);
    int (*kv_prefix)(const uint8_t *prefix, int prefix_len,
                     const uint8_t *cursor, int cursor_len, int limit,
                     int *out_outcome, uint8_t **out_json, int *out_json_len,
                     void *user);
    int (*module_load)(const uint8_t *spec, int spec_len,
                       uint8_t **out_src, int *out_src_len, void *user);
} arena_replay_host;

/* Register the native tape responder. Pass NULL to clear it (every
 * channel then reports "tape not installed"). `user` is handed back to
 * each callback unchanged. The struct is copied, so it need not outlive
 * the call. */
void arena_replay_set_host(const arena_replay_host *host, void *user);
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
