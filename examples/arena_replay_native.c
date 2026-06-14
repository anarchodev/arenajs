/*
 * arena_replay_native — replay a recorded request entirely in native C.
 *
 * This is the input side of the native host surface: where the browser
 * scrubber feeds recorded values from Module.tapes, a native driver
 * registers an arena_replay_host responder (qjs-arena-replay-bindings.h)
 * and serves module source + kv reads itself. Combined with the trace
 * sink (qjs-arena-trace.h, the output side) this is everything a native
 * trace-playing / simulation CLI needs from this repo — Date.now and the
 * PRNG are pinned separately and need no callback.
 *
 * The "tapes" here are two tiny in-memory maps. A real driver would
 * decode them from an RTAP blob, but the hook surface is identical: the
 * engine asks, the host answers. To *simulate* rather than replay, the
 * host simply answers with a value the recording didn't contain.
 *
 * Build (needs the trace emitter compiled in for the trace sink):
 *   cmake -B build -DQJS_BUILD_EXAMPLES=ON
 *   cmake --build build --target arena_replay_native
 *   ./build/arena_replay_native
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qjs-arena-trace.h"            /* arena_trace_set_host (output side) */
#include "qjs-arena-replay-bindings.h"  /* arena_replay_host  (input side)   */

/* Reactor C ABI (qjs-arena-reactor.c ships no header). */
extern int  arena_init(int base_kb, int request_kb);
extern int  arena_run_module(const char *entry_name, const char *entry_src);
extern void arena_set_trace_mode(int mode);
extern void arena_set_date_now(uint32_t lo, uint32_t hi);
extern void arena_set_random_seed(uint32_t lo, uint32_t hi);
extern void arena_destroy(void);

/* ── the "tapes": trivial in-memory maps ──────────────────────────────── */

struct kv { const char *key, *val; };
static struct kv g_kv[] = {
    { "user", "ada" },
};

struct mod { const char *spec, *src; };
static struct mod g_mods[] = {
    { "greet.js", "export function greet(n) { return 'hi ' + n; }\n" },
};

/* malloc + copy helper matching the responder contract: the engine frees
 * the buffer after copying it into a QJS value. `nul` adds a trailing NUL
 * (required for module source and kv.prefix JSON, where the parser reads
 * one byte past `len`). */
static uint8_t *dup_bytes(const char *s, int len, int nul) {
    uint8_t *p = malloc((size_t)len + (nul ? 1 : 0));
    if (!p) return NULL;
    memcpy(p, s, len);
    if (nul) p[len] = 0;
    return p;
}

/* ── replay host responders ───────────────────────────────────────────── */

static int kv_get(const uint8_t *key, int key_len,
                  int *out_outcome, uint8_t **out_val, int *out_val_len,
                  void *user) {
    (void)user;
    for (size_t i = 0; i < sizeof g_kv / sizeof *g_kv; i++) {
        if ((int)strlen(g_kv[i].key) == key_len &&
            memcmp(g_kv[i].key, key, key_len) == 0) {
            int vlen = (int)strlen(g_kv[i].val);
            *out_val = dup_bytes(g_kv[i].val, vlen, 0);
            *out_val_len = vlen;
            *out_outcome = 0;                 /* ok */
            printf("  [kv.get %.*s -> %s]\n", key_len, key, g_kv[i].val);
            return 0;
        }
    }
    *out_outcome = 1;                          /* not_found -> kv.get() == null */
    printf("  [kv.get %.*s -> (not found)]\n", key_len, key);
    return 0;
}

static int kv_set(const uint8_t *key, int key_len,
                  const uint8_t *val, int val_len, int *out_outcome,
                  void *user) {
    (void)user;
    /* A replay would verify this write against the recorded output set; a
     * simulator might capture it. Here we just surface the computed value. */
    printf("  [kv.set %.*s = %.*s]\n", key_len, key, val_len, val);
    *out_outcome = 0;
    return 0;
}

static int module_load(const uint8_t *spec, int spec_len,
                       uint8_t **out_src, int *out_src_len, void *user) {
    (void)user;
    for (size_t i = 0; i < sizeof g_mods / sizeof *g_mods; i++) {
        if ((int)strlen(g_mods[i].spec) == spec_len &&
            memcmp(g_mods[i].spec, spec, spec_len) == 0) {
            int slen = (int)strlen(g_mods[i].src);
            *out_src = dup_bytes(g_mods[i].src, slen, 1);   /* NUL-terminated */
            *out_src_len = slen;
            printf("  [module %.*s -> %d bytes]\n", spec_len, spec, slen);
            return 0;
        }
    }
    printf("  [module %.*s -> MISSING]\n", spec_len, spec);
    return -5;                                 /* divergence: off-tape import */
}

/* ── trace sink (output side), minimal: just function entries/throws ───── */

static int on_event(int kind, const uint8_t *p, int len, void *user) {
    (void)len; (void)user;
    if (kind == 1)        /* FUNC_ENTER: [name_atom][file_atom][line] */
        printf("  [enter fn @ line %u]\n",
               (uint32_t)(p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24));
    else if (kind == 4)   /* THROW */
        printf("  [throw]\n");
    return 0;             /* continue */
}

static const char *ENTRY =
    "import { greet } from 'greet.js';\n"
    "const who = kv.get('user');\n"
    "kv.set('greeting', greet(who ?? 'world'));\n";

int main(void) {
    if (arena_init(4096, 2048) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 1;
    }

    /* Input side: serve the recorded tapes. */
    arena_replay_host host = {
        .kv_get = kv_get,
        .kv_set = kv_set,
        .module_load = module_load,
        /* kv_delete / kv_prefix unused here -> NULL -> "tape not installed" */
    };
    arena_replay_set_host(&host, NULL);

    /* Pin the non-tape inputs to their recorded values (no callback). */
    arena_set_date_now(0, 0);
    arena_set_random_seed(1, 0);

    /* Output side: observe execution. */
    arena_trace_set_host(on_event, NULL, NULL);
    arena_set_trace_mode(1 /* SCAN */);

    printf("== replay ==\n");
    int rc = arena_run_module("main.js", ENTRY);
    printf("== done (rc=%d) ==\n", rc);

    arena_destroy();
    return rc < 0 ? 1 : 0;
}
