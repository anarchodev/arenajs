/*
 * arenajs replay-mode JS bindings — see qjs-arena-replay-bindings.h.
 *
 * Each globalThis surface gets a native QJS function. Each native
 * function calls an EM_JS-declared host import that advances the
 * relevant tape cursor and returns the recorded value. Divergence
 * (wrong key, wrong length, exhausted tape) is signalled by the host
 * returning a negative code which the native binding turns into a QJS
 * exception with a descriptive message.
 *
 * Outcome codes for kv operations mirror src/tape/root.zig's KvOutcome:
 *   0 = ok, 1 = not_found, 2 = err.
 */

#include "qjs-arena-replay-bindings.h"
#include "qjs-arena-reactor.h"   /* arena_entry_module */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ───────────── host imports (C → JS) ───────────── */
/*
 * All host imports follow the same shape:
 *   - Walk to Module.tapes.<channel>; if missing, return error code 1.
 *   - Initialize ._cursor to 0 lazily on first access.
 *   - If cursor is past the array end, return error code 2 (exhausted).
 *   - Consume the next entry, advance cursor, return value(s).
 *   - Negative non-1/2 codes signal divergence (wrong op, wrong key/len).
 *
 * Pointer out-params: writes to HEAP32[ptr >> 2].
 * Bytes out-params: host malloc's via Module._malloc, native copies + frees.
 */

/* docs/primitive-gaps.md §9 + §9 fold-in — Math.random, crypto.*,
 * AND Date.now draw from per-context state (xorshift64star +
 * `date_now_pinned`), not host tapes. Replay reseeds via
 * `arena_set_random_seed` and pins Date.now via
 * `arena_set_date_now` before running the handler. No host
 * callbacks for any of them. */

/* rove §8 (docs/primitive-gaps.md): minimal kv read set. The
   capture-side tape now records ONLY foreign reads — own-reads
   (a kv.get of a key the same activation just wrote) resolve
   against the activation's writeset overlay, and writes
   (kv.set / kv.delete) are outputs not inputs. Replay maintains
   an in-engine overlay (Module._kvOverlay, a Map keyed by string,
   value = string for set, null for delete tombstone) populated by
   the host's kv.set / kv.delete callbacks. kv.get checks the
   overlay first; on miss it falls through to the existing tape-
   consume path (the foreign-read entry the capture-side did
   tape). Reset between replays via the JS-side bootloader. RTAP
   wire version bumped 1 → 2 to signal the new semantics. */

/* On wasm32 the pointer params below marshal to JS as numeric addresses
   (the EM_JS bodies index HEAPU8/HEAP32 with them, exactly as when they
   were declared `int`). On native the matching dispatchers in the #else
   branch receive genuine pointers — so a 64-bit native host never sees a
   truncated address. The native sink lives after the last import. */
#ifdef __EMSCRIPTEN__

EM_JS(int, _arena_host_kv_get,
      (const uint8_t *key_ptr, int key_len,
       int *out_outcome_ptr, uint8_t **out_val_ptr_ptr, int *out_val_len_ptr), {
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const key = dec.decode(HEAPU8.subarray(key_ptr, key_ptr + key_len));
    const overlay = Module._kvOverlay || (Module._kvOverlay = new Map());
    if (overlay.has(key)) {
        const ov = overlay.get(key);
        if (ov === null) {
            HEAP32[out_outcome_ptr >> 2] = 1; /* not_found */
            HEAP32[out_val_ptr_ptr >> 2] = 0;
            HEAP32[out_val_len_ptr >> 2] = 0;
        } else {
            const enc = Module._tapeEnc || (Module._tapeEnc = new TextEncoder());
            const bytes = enc.encode(ov);
            const ptr = Module._malloc(bytes.length || 1);
            if (bytes.length) HEAPU8.set(bytes, ptr);
            HEAP32[out_outcome_ptr >> 2] = 0; /* ok */
            HEAP32[out_val_ptr_ptr >> 2] = ptr;
            HEAP32[out_val_len_ptr >> 2] = bytes.length;
        }
        return 0;
    }
    const t = Module.tapes && Module.tapes.kv;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    if (e.op !== 0) return -3;
    if (e.key !== key) return -4;
    HEAP32[out_outcome_ptr >> 2] = e.outcome;
    if (e.outcome === 0) {
        const enc = Module._tapeEnc || (Module._tapeEnc = new TextEncoder());
        const bytes = enc.encode(e.value);
        const ptr = Module._malloc(bytes.length || 1);
        if (bytes.length) HEAPU8.set(bytes, ptr);
        HEAP32[out_val_ptr_ptr >> 2] = ptr;
        HEAP32[out_val_len_ptr >> 2] = bytes.length;
    } else {
        HEAP32[out_val_ptr_ptr >> 2] = 0;
        HEAP32[out_val_len_ptr >> 2] = 0;
    }
    return 0;
});

EM_JS(int, _arena_host_kv_set,
      (const uint8_t *key_ptr, int key_len, const uint8_t *val_ptr, int val_len,
       int *out_outcome_ptr), {
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const key = dec.decode(HEAPU8.subarray(key_ptr, key_ptr + key_len));
    const val = dec.decode(HEAPU8.subarray(val_ptr, val_ptr + val_len));
    const overlay = Module._kvOverlay || (Module._kvOverlay = new Map());
    overlay.set(key, val);
    HEAP32[out_outcome_ptr >> 2] = 0; /* ok */
    return 0;
});

EM_JS(int, _arena_host_kv_delete,
      (const uint8_t *key_ptr, int key_len, int *out_outcome_ptr), {
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const key = dec.decode(HEAPU8.subarray(key_ptr, key_ptr + key_len));
    const overlay = Module._kvOverlay || (Module._kvOverlay = new Map());
    overlay.set(key, null); /* null = delete tombstone */
    HEAP32[out_outcome_ptr >> 2] = 0; /* ok */
    return 0;
});

/* module loader fetches source for a specifier. The host looks up
   Module.module_sources[specifier], falling back to
   Module.module_sources[source_hash_hex] — either keying works, the
   first matches rove's path-keyed deployment manifest, the second
   matches a hash-content-addressed store. Host malloc's the bytes;
   the native loader copies into a QJS string and frees. */
EM_JS(int, _arena_host_module_load,
      (const uint8_t *spec_ptr, int spec_len,
       uint8_t **out_src_ptr, int *out_src_len), {
    const t = Module.tapes && Module.tapes.module;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const spec = dec.decode(HEAPU8.subarray(spec_ptr, spec_ptr + spec_len));
    if (e.specifier !== spec) return -3;
    if (!Module.module_sources) return -4;
    const src = Module.module_sources[spec] !== undefined
        ? Module.module_sources[spec]
        : Module.module_sources[e.source_hash_hex];
    if (src === undefined) return -5;
    const enc = Module._tapeEnc || (Module._tapeEnc = new TextEncoder());
    const bytes = typeof src === "string" ? enc.encode(src) : src;
    // JS_Eval takes (input, input_len) but the parser still expects a
    // trailing NUL — without it, parsing reads garbage past the end and
    // surfaces as bogus syntax errors (the symptom depends on whatever
    // happens to be in the next byte of the heap).
    const ptr = Module._malloc(bytes.length + 1);
    if (bytes.length) HEAPU8.set(bytes, ptr);
    HEAPU8[ptr + bytes.length] = 0;
    HEAP32[out_src_ptr >> 2] = ptr;
    HEAP32[out_src_len >> 2] = bytes.length;
    return 0;
});

/* kv.prefix returns an array of {key, value}. To avoid designing a
   per-row ABI, the host returns a JSON-encoded array which the native
   binding parses via JS_ParseJSON. Result rows fit comfortably in
   memory for any realistic capture (prefix scan is capped by .limit). */
EM_JS(int, _arena_host_kv_prefix,
      (const uint8_t *prefix_ptr, int prefix_len,
       const uint8_t *cursor_ptr, int cursor_len, int limit,
       int *out_outcome_ptr, uint8_t **out_json_ptr_ptr, int *out_json_len_ptr), {
    const t = Module.tapes && Module.tapes.kv;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    if (e.op !== 3) return -3;
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const prefix = dec.decode(HEAPU8.subarray(prefix_ptr, prefix_ptr + prefix_len));
    const cur = cursor_len === 0 ? "" :
                dec.decode(HEAPU8.subarray(cursor_ptr, cursor_ptr + cursor_len));
    if (e.key !== prefix) return -4;
    if (e.cursor !== cur) return -5;
    if (e.limit !== limit) return -6;
    HEAP32[out_outcome_ptr >> 2] = e.outcome;
    const rows = (e.results || []).map(p => ({
        key:   typeof p.key   === "string" ? p.key   : dec.decode(p.key),
        value: typeof p.value === "string" ? p.value : dec.decode(p.value),
    }));
    const enc = Module._tapeEnc || (Module._tapeEnc = new TextEncoder());
    const bytes = enc.encode(JSON.stringify(rows));
    // Same NUL-terminator requirement as the module-load buffer:
    // JS_ParseJSON scans with a length-aware parser but still expects
    // a sentinel past the last byte.
    const ptr = Module._malloc(bytes.length + 1);
    if (bytes.length) HEAPU8.set(bytes, ptr);
    HEAPU8[ptr + bytes.length] = 0;
    HEAP32[out_json_ptr_ptr >> 2] = ptr;
    HEAP32[out_json_len_ptr >> 2] = bytes.length;
    return 0;
});

#else  /* native: dispatch each tape read to the registered host responder */

/* Copy of the embedder's responder table (see qjs-arena-replay-bindings.h).
   Zero-initialised, so before arena_replay_set_host() every channel is NULL
   and reports "tape not installed" (code 1) — the same outcome the browser
   host gives when Module.tapes.<channel> is absent. */
static arena_replay_host s_host;
static void              *s_host_user = NULL;

void arena_replay_set_host(const arena_replay_host *host, void *user)
{
    if (host) s_host = *host;
    else      memset(&s_host, 0, sizeof s_host);
    s_host_user = user;
}

static int _arena_host_kv_get(const uint8_t *key, int key_len,
                              int *out_outcome, uint8_t **out_val,
                              int *out_val_len)
{
    if (!s_host.kv_get) return 1;
    return s_host.kv_get(key, key_len, out_outcome, out_val, out_val_len,
                         s_host_user);
}

static int _arena_host_kv_set(const uint8_t *key, int key_len,
                              const uint8_t *val, int val_len, int *out_outcome)
{
    if (!s_host.kv_set) return 1;
    return s_host.kv_set(key, key_len, val, val_len, out_outcome, s_host_user);
}

static int _arena_host_kv_delete(const uint8_t *key, int key_len,
                                 int *out_outcome)
{
    if (!s_host.kv_delete) return 1;
    return s_host.kv_delete(key, key_len, out_outcome, s_host_user);
}

static int _arena_host_kv_prefix(const uint8_t *prefix, int prefix_len,
                                 const uint8_t *cursor, int cursor_len, int limit,
                                 int *out_outcome, uint8_t **out_json,
                                 int *out_json_len)
{
    if (!s_host.kv_prefix) return 1;
    return s_host.kv_prefix(prefix, prefix_len, cursor, cursor_len, limit,
                            out_outcome, out_json, out_json_len, s_host_user);
}

static int _arena_host_module_load(const uint8_t *spec, int spec_len,
                                   uint8_t **out_src, int *out_src_len)
{
    if (!s_host.module_load) return 1;
    return s_host.module_load(spec, spec_len, out_src, out_src_len, s_host_user);
}

#endif  /* __EMSCRIPTEN__ */

/* ───────────── native QJS bindings (JS → C → host) ───────────── */

static JSValue throw_tape_error(JSContext *ctx, const char *channel, int rc)
{
    if (rc == 1)
        return JS_ThrowReferenceError(ctx, "%s tape not installed", channel);
    if (rc == 2)
        return JS_ThrowInternalError(ctx, "%s tape exhausted", channel);
    return JS_ThrowInternalError(ctx, "%s tape diverged (rc=%d)", channel, rc);
}

/* §9 fold-in: Date.now / new Date() are handled by arenajs natively
 * via `date_now_pinned` set with JS_SetDateNow. No host hook needed. */

/* docs/primitive-gaps.md §9 — crypto.* draws from the per-request
 * PRNG state (xorshift64star) via JS_FillRandomBytes, sharing the
 * stream with Math.random. Replay reproduces by re-seeding the
 * same arenajs PRNG; no tape, no host callback. */

static JSValue jsb_crypto_getRandomValues(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getRandomValues: requires 1 argument");
    size_t byte_offset = 0, byte_length = 0, bytes_per_element = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset,
                                          &byte_length, &bytes_per_element);
    if (JS_IsException(buf))
        return buf;
    size_t ab_size = 0;
    uint8_t *ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, buf);
    JS_FreeValue(ctx, buf);
    if (!ab_ptr) {
        return JS_ThrowTypeError(ctx, "getRandomValues: bad backing buffer");
    }
    JS_FillRandomBytes(ctx, ab_ptr + byte_offset, byte_length);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue jsb_crypto_randomBytes(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    int32_t n = 0;
    if (argc < 1 || JS_ToInt32(ctx, &n, argv[0]) < 0 || n < 0)
        return JS_ThrowTypeError(ctx, "randomBytes: expected non-negative integer");
    uint8_t *buf = (uint8_t *)js_malloc(ctx, (size_t)n + 1);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    JS_FillRandomBytes(ctx, buf, (size_t)n);
    JSValue ta = JS_NewUint8ArrayCopy(ctx, buf, (size_t)n);
    js_free(ctx, buf);
    return ta;
}

static JSValue jsb_crypto_randomUUID(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    uint8_t b[16];
    JS_FillRandomBytes(ctx, b, 16);
    /* RFC 4122 v4 — set the version + variant nibbles. */
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    char out[37];
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 0xf];
    }
    out[36] = '\0';
    return JS_NewStringLen(ctx, out, 36);
}

/* ── SHA-256 + HMAC-SHA256 ───────────────────────────────────────────────
 * Embedded (public-domain SHA-256) so the replay/sim base provides the
 * hashing the worker's `_system.crypto` does — `globals/crypto.js` composes
 * `crypto.sha256`/`hmacSha256` (lowercase-hex over a string or Uint8Array)
 * into magic-link / session / webhook-idempotency machinery. No OpenSSL, so
 * this stays in the portable `arenajs-replay` the `rewind` CLI cross-compiles.
 * (Streaming sha256Init/Update/Final + RSA/ECDSA are NOT here yet.) */
#define ARENA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
typedef struct { uint32_t s[8]; uint64_t n; uint8_t buf[64]; size_t len; } arena_sha256;

static void arena_sha256_init(arena_sha256 *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
        0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->s, iv, sizeof iv);
    c->n = 0;
    c->len = 0;
}

static void arena_sha256_block(arena_sha256 *c, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ARENA_ROR(w[i-15], 7) ^ ARENA_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ARENA_ROR(w[i-2], 17) ^ ARENA_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ARENA_ROR(e,6) ^ ARENA_ROR(e,11) ^ ARENA_ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ARENA_ROR(a,2) ^ ARENA_ROR(a,13) ^ ARENA_ROR(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d; c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}

static void arena_sha256_update(arena_sha256 *c, const uint8_t *p, size_t n)
{
    c->n += n;
    while (n) {
        size_t take = 64 - c->len;
        if (take > n) take = n;
        memcpy(c->buf + c->len, p, take);
        c->len += take; p += take; n -= take;
        if (c->len == 64) { arena_sha256_block(c, c->buf); c->len = 0; }
    }
}

static void arena_sha256_final(arena_sha256 *c, uint8_t out[32])
{
    uint64_t bits = c->n * 8;
    uint8_t pad = 0x80, z = 0;
    arena_sha256_update(c, &pad, 1);
    while (c->len != 56) arena_sha256_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i*8));
    arena_sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->s[i] >> 24);
        out[i*4+1] = (uint8_t)(c->s[i] >> 16);
        out[i*4+2] = (uint8_t)(c->s[i] >> 8);
        out[i*4+3] = (uint8_t)(c->s[i]);
    }
}

static void arena_hmac_sha256(const uint8_t *key, size_t kl,
                              const uint8_t *msg, size_t ml, uint8_t out[32])
{
    uint8_t k[64] = {0};
    if (kl > 64) { arena_sha256 c; arena_sha256_init(&c); arena_sha256_update(&c, key, kl); arena_sha256_final(&c, k); }
    else memcpy(k, key, kl);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t ih[32];
    arena_sha256 c;
    arena_sha256_init(&c); arena_sha256_update(&c, ipad, 64); arena_sha256_update(&c, msg, ml); arena_sha256_final(&c, ih);
    arena_sha256_init(&c); arena_sha256_update(&c, opad, 64); arena_sha256_update(&c, ih, 32); arena_sha256_final(&c, out);
}

/* Extract a string (UTF-8) or Uint8Array arg into a malloc'd byte buffer. */
static uint8_t *arena_crypto_arg_bytes(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    if (JS_IsString(v)) {
        size_t l;
        const char *s = JS_ToCStringLen(ctx, &l, v);
        if (!s) return NULL;
        uint8_t *b = malloc(l ? l : 1);
        if (b) memcpy(b, s, l);
        JS_FreeCString(ctx, s);
        *out_len = l;
        return b;
    }
    size_t off = 0, len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) return NULL;
    size_t ab = 0;
    uint8_t *ap = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!ap) return NULL;
    uint8_t *b = malloc(len ? len : 1);
    if (b) memcpy(b, ap + off, len);
    *out_len = len;
    return b;
}

static JSValue arena_hex32(JSContext *ctx, const uint8_t d[32])
{
    static const char h[] = "0123456789abcdef";
    char o[64];
    for (int i = 0; i < 32; i++) { o[i*2] = h[d[i] >> 4]; o[i*2+1] = h[d[i] & 0xf]; }
    return JS_NewStringLen(ctx, o, 64);
}

static JSValue jsb_crypto_sha256(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha256 requires 1 argument");
    size_t l = 0;
    uint8_t *b = arena_crypto_arg_bytes(ctx, argv[0], &l);
    if (!b) return JS_EXCEPTION;
    arena_sha256 c;
    uint8_t d[32];
    arena_sha256_init(&c); arena_sha256_update(&c, b, l); arena_sha256_final(&c, d);
    free(b);
    return arena_hex32(ctx, d);
}

static JSValue jsb_crypto_hmacSha256(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "crypto.hmacSha256 requires (key, data)");
    size_t kl = 0, dl = 0;
    uint8_t *k = arena_crypto_arg_bytes(ctx, argv[0], &kl);
    if (!k) return JS_EXCEPTION;
    uint8_t *dt = arena_crypto_arg_bytes(ctx, argv[1], &dl);
    if (!dt) { free(k); return JS_EXCEPTION; }
    uint8_t d[32];
    arena_hmac_sha256(k, kl, dt, dl, d);
    free(k); free(dt);
    return arena_hex32(ctx, d);
}

/* kv.get(key) — returns the recorded string value, null for not_found,
   throws for err (so handler error-handling under replay matches prod). */
static JSValue jsb_kv_get(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "kv.get: requires 1 argument");
    size_t key_len = 0;
    const char *key = JS_ToCStringLen(ctx, &key_len, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    int outcome = 0, val_len = 0;
    uint8_t *val = NULL;
    int rc = _arena_host_kv_get((const uint8_t *)key, (int)key_len,
                                 &outcome, &val, &val_len);
    JS_FreeCString(ctx, key);
    if (rc != 0)
        return throw_tape_error(ctx, "kv", rc);
    if (outcome == 1) {  /* not_found */
        free(val);
        return JS_NULL;
    }
    if (outcome == 2) {  /* err */
        free(val);
        return JS_ThrowInternalError(ctx, "kv.get: recorded failure");
    }
    JSValue v = JS_NewStringLen(ctx, (const char *)val, (size_t)val_len);
    free(val);
    return v;
}

/* kv.set(key, value) — returns undefined; throws for err. */
static JSValue jsb_kv_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "kv.set: requires 2 arguments");
    size_t key_len = 0, val_len = 0;
    const char *key = JS_ToCStringLen(ctx, &key_len, argv[0]);
    if (!key) return JS_EXCEPTION;
    const char *val = JS_ToCStringLen(ctx, &val_len, argv[1]);
    if (!val) { JS_FreeCString(ctx, key); return JS_EXCEPTION; }
    int outcome = 0;
    int rc = _arena_host_kv_set((const uint8_t *)key, (int)key_len,
                                 (const uint8_t *)val, (int)val_len, &outcome);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);
    if (rc != 0)
        return throw_tape_error(ctx, "kv", rc);
    if (outcome == 2)
        return JS_ThrowInternalError(ctx, "kv.set: recorded failure");
    return JS_UNDEFINED;
}

/* kv.delete(key) — returns undefined; throws for err. not_found is
   indistinguishable from ok at the API surface, but the recorded
   outcome is still consumed. */
static JSValue jsb_kv_delete(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "kv.delete: requires 1 argument");
    size_t key_len = 0;
    const char *key = JS_ToCStringLen(ctx, &key_len, argv[0]);
    if (!key) return JS_EXCEPTION;
    int outcome = 0;
    int rc = _arena_host_kv_delete((const uint8_t *)key, (int)key_len, &outcome);
    JS_FreeCString(ctx, key);
    if (rc != 0)
        return throw_tape_error(ctx, "kv", rc);
    if (outcome == 2)
        return JS_ThrowInternalError(ctx, "kv.delete: recorded failure");
    return JS_UNDEFINED;
}

/* kv.prefix(prefix, opts?) — returns an array of {key, value} matching
   the recorded scan. opts.cursor and opts.limit are recorded too so
   the replay verifies the handler called with the same arguments. */
static JSValue jsb_kv_prefix(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "kv.prefix: requires prefix argument");
    size_t prefix_len = 0;
    const char *prefix = JS_ToCStringLen(ctx, &prefix_len, argv[0]);
    if (!prefix) return JS_EXCEPTION;

    const char *cursor_buf = NULL;   /* malloc'd via JS_ToCStringLen, or NULL */
    const char *cursor_arg = "";     /* what to ship to the host */
    size_t cursor_len = 0;
    int32_t limit = 0;
    /* POSITIONAL (prefix, cursor, limit) — the shape the embedder's live
       binding uses (rove src/js/globals_kv.zig jsKvPrefix, argc=3), so a
       handler or shim that pages kv issues the identical call live and on
       replay. An options-bag spelling here would silently read limit=0
       from a positional caller, which a strict tape then rejects as a
       divergence at the paging call — invisible against a map-backed
       host, fatal against a recording. */
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        cursor_buf = JS_ToCStringLen(ctx, &cursor_len, argv[1]);
        if (!cursor_buf) {
            JS_FreeCString(ctx, prefix);
            return JS_EXCEPTION;
        }
        cursor_arg = cursor_buf;
    }
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
        JS_ToInt32(ctx, &limit, argv[2]);

    int outcome = 0, json_len = 0;
    uint8_t *json = NULL;
    int rc = _arena_host_kv_prefix(
        (const uint8_t *)prefix, (int)prefix_len,
        (const uint8_t *)cursor_arg, (int)cursor_len, limit,
        &outcome, &json, &json_len);
    JS_FreeCString(ctx, prefix);
    if (cursor_buf) JS_FreeCString(ctx, cursor_buf);
    if (rc != 0)
        return throw_tape_error(ctx, "kv", rc);
    if (outcome == 2) {
        free(json);
        return JS_ThrowInternalError(ctx, "kv.prefix: recorded failure");
    }
    JSValue arr = JS_ParseJSON(ctx, (const char *)json,
                                (size_t)json_len, "<kv.prefix>");
    free(json);
    return arr;
}

/* ───────────── module loader ───────────── */

static JSModuleDef *jsb_module_loader(JSContext *ctx, const char *module_name,
                                       void *opaque)
{
    (void)opaque;
    uint8_t *src = NULL;
    int src_len = 0;
    int rc = _arena_host_module_load(
        (const uint8_t *)module_name, (int)strlen(module_name),
        &src, &src_len);
    if (rc != 0) {
        throw_tape_error(ctx, "module", rc);
        return NULL;
    }
    JSValue compiled = JS_Eval(ctx, (const char *)src,
                                (size_t)src_len, module_name,
                                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    if (JS_IsException(compiled))
        return NULL;
    /* The compiled value is a function-wrapper for the module def;
       we take the underlying ptr and free the wrapper. The module def
       itself is kept live by the runtime's module registry. */
    JSModuleDef *m = JS_VALUE_GET_PTR(compiled);
    JS_FreeValue(ctx, compiled);
    return m;
}

int arena_install_replay_module_loader(JSRuntime *rt)
{
    /* module_normalize=NULL → QJS uses js_default_module_normalize_name,
       which handles leading ./ and ../ the same way Node's resolver does
       (and the same way rove's web/replay/app.js's resolveRelative does). */
    JS_SetModuleLoaderFunc(rt, NULL, jsb_module_loader, NULL);
    return 0;
}

/* Same tape-backed loader, but with a caller-supplied normalizer + opaque.
   Lets an embedder resolve bare specifiers (e.g. `@scope/pkg` package imports)
   before the loader pulls source, while keeping the proven source-serving
   loader unchanged. `normalize` must fully resolve — including ./ and ../ —
   since it replaces the default normalizer; `opaque` is passed to `normalize`
   (the loader itself uses the process-global replay host). Must be called
   pre-freeze. Returns 0. */
int arena_install_replay_module_loader_normalize(JSRuntime *rt,
                                                 JSModuleNormalizeFunc *normalize,
                                                 void *opaque)
{
    JS_SetModuleLoaderFunc(rt, normalize, jsb_module_loader, opaque);
    return 0;
}

/* ───────────── entry-module namespace ───────────── */

/* `__arena_entry_ns()` — the namespace object of the entry module of
   the CURRENT arena_run_module call on THIS runtime (resolved per
   reactor instance via arena_entry_module, qjs-arena-reactor.h). Lets
   a replay epilogue appended to the entry source invoke the module's
   exports through the namespace — including an anonymous
   `export default`, which has no module-scope binding and is otherwise
   unreachable from appended code (self-imports route through the host
   loader and diverge from the module tape). Returns undefined when
   called outside a module run. */
static JSValue jsb_entry_ns(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSModuleDef *m = arena_entry_module(ctx);
    if (!m)
        return JS_UNDEFINED;
    return JS_GetModuleNamespace(ctx, m);
}

/* ───────────── registration ───────────── */

int arena_install_replay_bindings(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    /* docs/primitive-gaps.md §9 — no Math.random / Date.now
     * overrides. arenajs's native `js_math_random` and `js_Date_now`
     * (+ `js_date_constructor` no-args path) run against per-context
     * state (xorshift64star + `date_now_pinned`), set per request by
     * the embedder via `JS_SetRandomSeed` / `JS_SetDateNow` (server)
     * or the WASM reactor `arena_set_random_seed` /
     * `arena_set_date_now` exports (replay shell). */

    /* crypto namespace */
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "getRandomValues",
        JS_NewCFunction(ctx, jsb_crypto_getRandomValues, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, crypto, "randomBytes",
        JS_NewCFunction(ctx, jsb_crypto_randomBytes, "randomBytes", 1));
    JS_SetPropertyStr(ctx, crypto, "randomUUID",
        JS_NewCFunction(ctx, jsb_crypto_randomUUID, "randomUUID", 0));
    JS_SetPropertyStr(ctx, crypto, "sha256",
        JS_NewCFunction(ctx, jsb_crypto_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256",
        JS_NewCFunction(ctx, jsb_crypto_hmacSha256, "hmacSha256", 2));
    JS_SetPropertyStr(ctx, global, "crypto", crypto);

    /* kv namespace */
    JSValue kv = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, kv, "get",
        JS_NewCFunction(ctx, jsb_kv_get, "get", 1));
    JS_SetPropertyStr(ctx, kv, "set",
        JS_NewCFunction(ctx, jsb_kv_set, "set", 2));
    JS_SetPropertyStr(ctx, kv, "delete",
        JS_NewCFunction(ctx, jsb_kv_delete, "delete", 1));
    JS_SetPropertyStr(ctx, kv, "prefix",
        JS_NewCFunction(ctx, jsb_kv_prefix, "prefix", 2));
    JS_SetPropertyStr(ctx, global, "kv", kv);

    /* Entry-module namespace accessor for the replay epilogue. */
    JS_SetPropertyStr(ctx, global, "__arena_entry_ns",
        JS_NewCFunction(ctx, jsb_entry_ns, "__arena_entry_ns", 0));

    JS_FreeValue(ctx, global);
    return 0;
}
