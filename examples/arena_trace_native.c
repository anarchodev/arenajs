/*
 * arena_trace_native — drive the arenajs reactor natively and print the
 * structural trace events through a C host callback.
 *
 * This is the native counterpart to what the browser build does with
 * Module.host_trace: the engine emits NAME / FUNC_ENTER / FUNC_EXIT /
 * LINE / THROW events into a scratch buffer and hands each to a host
 * callback. Here the callback decodes the little-endian wire format and
 * prints a human-readable trace.
 *
 * Build: this needs the trace emitter compiled in, which is OFF by
 * default. The CMake target `arena_trace_native` sets ARENA_TRACE_ENABLED=1
 * (see CMakeLists.txt) and links the reactor + replay bindings + core.
 *
 *   cmake -B build -DQJS_BUILD_EXAMPLES=ON
 *   cmake --build build --target arena_trace_native
 *   ./build/arena_trace_native
 *
 * See the wire format and return-code contract in qjs-arena-trace.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qjs-arena-trace.h"   /* arena_trace_set_host, event/state fn types */

/* The reactor (qjs-arena-reactor.c) exposes a flat C ABI but ships no
 * header — declare the entry points we use. */
extern int  arena_init(int base_kb, int request_kb);
extern int  arena_run_module(const char *entry_name, const char *entry_src);
extern void arena_set_trace_mode(int mode);
extern void arena_destroy(void);

/* Mirrors qjs-arena-trace.h: OFF / SCAN (func enter+exit+throw) /
 * DRILL (also per-line). */
#define TRACE_OFF   0
#define TRACE_SCAN  1
#define TRACE_DRILL 2

/* Little-endian readers. The payload is emitted in the engine's native
 * byte order; this example assumes a little-endian host (x86, arm64).
 * A big-endian host would byte-swap here. */
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

/* id→name table, populated by NAME events. Atoms are small and interned,
 * so a flat array keyed by a bounded slot count is plenty for a demo. */
#define NAMES_CAP 1024
static struct { uint32_t atom; char str[128]; } g_names[NAMES_CAP];
static int g_names_n;

static const char *name_of(uint32_t atom) {
    for (int i = 0; i < g_names_n; i++)
        if (g_names[i].atom == atom) return g_names[i].str;
    return "?";
}

static void remember_name(uint32_t atom, const uint8_t *s, int slen) {
    if (g_names_n >= NAMES_CAP) return;
    if (slen > (int)sizeof g_names[0].str - 1) slen = sizeof g_names[0].str - 1;
    g_names[g_names_n].atom = atom;
    memcpy(g_names[g_names_n].str, s, slen);
    g_names[g_names_n].str[slen] = '\0';
    g_names_n++;
}

static int g_depth;   /* indent FUNC_ENTER/EXIT nesting */

/* The host trace sink. Decode by `kind` per the qjs-arena-trace.h wire
 * format. Return 0 (continue) throughout — this demo never stops the run;
 * return 1 to stop, or 2 to stop and receive a stack snapshot in on_state. */
static int on_event(int kind, const uint8_t *p, int len, void *user) {
    (void)user; (void)len;
    switch (kind) {
    case 0: /* NAME: [atom u32][len u16][utf8] */
        remember_name(rd_u32(p), p + 6, rd_u16(p + 4));
        break;
    case 1: { /* FUNC_ENTER: [name_atom u32][file_atom u32][line u32] */
        uint32_t fn = rd_u32(p), file = rd_u32(p + 4), line = rd_u32(p + 8);
        printf("%*s→ %s  (%s:%u)\n", g_depth * 2, "",
               name_of(fn), name_of(file), line);
        g_depth++;
        break;
    }
    case 2: /* FUNC_EXIT: no payload */
        if (g_depth) g_depth--;
        printf("%*s← return\n", g_depth * 2, "");
        break;
    case 3: { /* LINE: [file_atom u32][line u32] */
        uint32_t file = rd_u32(p), line = rd_u32(p + 4);
        printf("%*s· %s:%u\n", g_depth * 2, "", name_of(file), line);
        break;
    }
    case 4: { /* THROW: [file_atom u32][line u32][len u16][utf8 msg] */
        uint32_t file = rd_u32(p), line = rd_u32(p + 4);
        int mlen = rd_u16(p + 8);
        printf("%*s✗ throw %.*s  (%s:%u)\n", g_depth * 2, "",
               mlen, (const char *)p + 10, name_of(file), line);
        break;
    }
    default:
        break;
    }
    return 0; /* continue */
}

/* Only reached if on_event returns 2. Prints the JSON stack snapshot. */
static void on_state(const uint8_t *json, int len, void *user) {
    (void)user;
    printf("-- stack snapshot --\n%.*s\n", len, (const char *)json);
}

static const char *SRC =
    "function add(a, b) {\n"
    "  const sum = a + b;\n"
    "  return sum;\n"
    "}\n"
    "function main() {\n"
    "  let total = 0;\n"
    "  for (let i = 1; i <= 3; i++) total = add(total, i);\n"
    "  return total;\n"
    "}\n"
    "globalThis.__result = main();\n";

int main(void) {
    if (arena_init(4096, 2048) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 1;
    }

    arena_trace_set_host(on_event, on_state, NULL);
    arena_set_trace_mode(TRACE_DRILL);   /* line-level detail */

    printf("== native trace ==\n");
    int rc = arena_run_module("main.js", SRC);
    printf("== done (rc=%d) ==\n", rc);

    arena_destroy();
    return rc < 0 ? 1 : 0;
}
