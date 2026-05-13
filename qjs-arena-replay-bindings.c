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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
/* Stubs so this file compiles on native (for IDE indexing / sanity
   builds); it's only linked into the WASM target in practice. */
#define EM_JS(ret, name, args, body) static ret name args { return (ret)0; }
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

EM_JS(double, _arena_host_math_random, (int *err_ptr), {
    const t = Module.tapes && Module.tapes.math_random;
    if (!t) { HEAP32[err_ptr >> 2] = 1; return 0.0; }
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) { HEAP32[err_ptr >> 2] = 2; return 0.0; }
    HEAP32[err_ptr >> 2] = 0;
    return t[t._cursor++].value;
});

EM_JS(double, _arena_host_date_now, (int *err_ptr), {
    const t = Module.tapes && Module.tapes.date;
    if (!t) { HEAP32[err_ptr >> 2] = 1; return 0.0; }
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) { HEAP32[err_ptr >> 2] = 2; return 0.0; }
    HEAP32[err_ptr >> 2] = 0;
    return t[t._cursor++].ms;
});

EM_JS(int, _arena_host_crypto_random,
      (int requested_len, int *out_ptr_ptr, int *out_len_ptr), {
    const t = Module.tapes && Module.tapes.crypto_random;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    if (e.bytes.byteLength !== requested_len) return -3;
    const ptr = Module._malloc(e.bytes.byteLength || 1);
    if (e.bytes.byteLength) HEAPU8.set(e.bytes, ptr);
    HEAP32[out_ptr_ptr >> 2] = ptr;
    HEAP32[out_len_ptr >> 2] = e.bytes.byteLength;
    return 0;
});

EM_JS(int, _arena_host_kv_get,
      (int key_ptr, int key_len,
       int *out_outcome_ptr, int *out_val_ptr_ptr, int *out_val_len_ptr), {
    const t = Module.tapes && Module.tapes.kv;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    if (e.op !== 0) return -3;
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const key = dec.decode(HEAPU8.subarray(key_ptr, key_ptr + key_len));
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
      (int key_ptr, int key_len, int val_ptr, int val_len,
       int *out_outcome_ptr), {
    const t = Module.tapes && Module.tapes.kv;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    if (e.op !== 1) return -3;
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const key = dec.decode(HEAPU8.subarray(key_ptr, key_ptr + key_len));
    const val = dec.decode(HEAPU8.subarray(val_ptr, val_ptr + val_len));
    if (e.key !== key) return -4;
    if (e.value !== val) return -5;
    HEAP32[out_outcome_ptr >> 2] = e.outcome;
    return 0;
});

EM_JS(int, _arena_host_kv_delete,
      (int key_ptr, int key_len, int *out_outcome_ptr), {
    const t = Module.tapes && Module.tapes.kv;
    if (!t) return 1;
    if (t._cursor === undefined) t._cursor = 0;
    if (t._cursor >= t.length) return 2;
    const e = t[t._cursor++];
    if (e.op !== 2) return -3;
    const dec = Module._tapeDec || (Module._tapeDec = new TextDecoder());
    const key = dec.decode(HEAPU8.subarray(key_ptr, key_ptr + key_len));
    if (e.key !== key) return -4;
    HEAP32[out_outcome_ptr >> 2] = e.outcome;
    return 0;
});

/* module loader fetches source for a specifier. The host looks up
   Module.module_sources[specifier], falling back to
   Module.module_sources[source_hash_hex] — either keying works, the
   first matches rove's path-keyed deployment manifest, the second
   matches a hash-content-addressed store. Host malloc's the bytes;
   the native loader copies into a QJS string and frees. */
EM_JS(int, _arena_host_module_load,
      (int spec_ptr, int spec_len,
       int *out_src_ptr, int *out_src_len), {
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
      (int prefix_ptr, int prefix_len,
       int cursor_ptr, int cursor_len, int limit,
       int *out_outcome_ptr, int *out_json_ptr_ptr, int *out_json_len_ptr), {
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

/* ───────────── native QJS bindings (JS → C → host) ───────────── */

static JSValue throw_tape_error(JSContext *ctx, const char *channel, int rc)
{
    if (rc == 1)
        return JS_ThrowReferenceError(ctx, "%s tape not installed", channel);
    if (rc == 2)
        return JS_ThrowInternalError(ctx, "%s tape exhausted", channel);
    return JS_ThrowInternalError(ctx, "%s tape diverged (rc=%d)", channel, rc);
}

static JSValue jsb_math_random(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int err = 0;
    double v = _arena_host_math_random(&err);
    if (err)
        return throw_tape_error(ctx, "math_random", err);
    return JS_NewFloat64(ctx, v);
}

static JSValue jsb_date_now(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int err = 0;
    double v = _arena_host_date_now(&err);
    if (err)
        return throw_tape_error(ctx, "date", err);
    /* Date.now returns an integer ms-since-epoch; tape stores it as an
       exact value so a plain JS number suffices. */
    return JS_NewInt64(ctx, (int64_t)v);
}

/* crypto.getRandomValues(typedArray) — fills the array in place and
   returns the same array. */
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
    int host_ptr = 0, host_len = 0;
    int rc = _arena_host_crypto_random((int)byte_length, &host_ptr, &host_len);
    if (rc != 0)
        return throw_tape_error(ctx, "crypto_random", rc);
    memcpy(ab_ptr + byte_offset, (void *)(intptr_t)host_ptr, (size_t)host_len);
    free((void *)(intptr_t)host_ptr);
    return JS_DupValue(ctx, argv[0]);
}

/* crypto.randomBytes(n) — returns a fresh Uint8Array filled with bytes. */
static JSValue jsb_crypto_randomBytes(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    int32_t n = 0;
    if (argc < 1 || JS_ToInt32(ctx, &n, argv[0]) < 0 || n < 0)
        return JS_ThrowTypeError(ctx, "randomBytes: expected non-negative integer");
    int host_ptr = 0, host_len = 0;
    int rc = _arena_host_crypto_random(n, &host_ptr, &host_len);
    if (rc != 0)
        return throw_tape_error(ctx, "crypto_random", rc);
    JSValue ta = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(intptr_t)host_ptr,
                                       (size_t)host_len);
    free((void *)(intptr_t)host_ptr);
    return ta;
}

/* crypto.randomUUID — formats 16 bytes as canonical UUID v4-style string.
   The bytes come from the tape exactly as the original call produced
   them, so we don't enforce the version/variant nibble layout — replay
   must mirror what was recorded byte-for-byte. */
static JSValue jsb_crypto_randomUUID(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int host_ptr = 0, host_len = 0;
    int rc = _arena_host_crypto_random(16, &host_ptr, &host_len);
    if (rc != 0)
        return throw_tape_error(ctx, "crypto_random", rc);
    if (host_len != 16) {
        free((void *)(intptr_t)host_ptr);
        return JS_ThrowInternalError(ctx, "randomUUID: tape returned %d bytes",
                                      host_len);
    }
    const uint8_t *b = (const uint8_t *)(intptr_t)host_ptr;
    char out[37];
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 0xf];
    }
    out[36] = '\0';
    free((void *)(intptr_t)host_ptr);
    return JS_NewStringLen(ctx, out, 36);
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
    int outcome = 0, val_ptr = 0, val_len = 0;
    int rc = _arena_host_kv_get((int)(intptr_t)key, (int)key_len,
                                 &outcome, &val_ptr, &val_len);
    JS_FreeCString(ctx, key);
    if (rc != 0)
        return throw_tape_error(ctx, "kv", rc);
    if (outcome == 1) {  /* not_found */
        if (val_ptr) free((void *)(intptr_t)val_ptr);
        return JS_NULL;
    }
    if (outcome == 2) {  /* err */
        if (val_ptr) free((void *)(intptr_t)val_ptr);
        return JS_ThrowInternalError(ctx, "kv.get: recorded failure");
    }
    JSValue v = JS_NewStringLen(ctx, (const char *)(intptr_t)val_ptr,
                                 (size_t)val_len);
    free((void *)(intptr_t)val_ptr);
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
    int rc = _arena_host_kv_set((int)(intptr_t)key, (int)key_len,
                                 (int)(intptr_t)val, (int)val_len, &outcome);
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
    int rc = _arena_host_kv_delete((int)(intptr_t)key, (int)key_len, &outcome);
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
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue cv = JS_GetPropertyStr(ctx, argv[1], "cursor");
        if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
            cursor_buf = JS_ToCStringLen(ctx, &cursor_len, cv);
            if (!cursor_buf) {
                JS_FreeValue(ctx, cv);
                JS_FreeCString(ctx, prefix);
                return JS_EXCEPTION;
            }
            cursor_arg = cursor_buf;
        }
        JS_FreeValue(ctx, cv);
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "limit");
        if (!JS_IsUndefined(lv))
            JS_ToInt32(ctx, &limit, lv);
        JS_FreeValue(ctx, lv);
    }

    int outcome = 0, json_ptr = 0, json_len = 0;
    int rc = _arena_host_kv_prefix(
        (int)(intptr_t)prefix, (int)prefix_len,
        (int)(intptr_t)cursor_arg, (int)cursor_len, limit,
        &outcome, &json_ptr, &json_len);
    JS_FreeCString(ctx, prefix);
    if (cursor_buf) JS_FreeCString(ctx, cursor_buf);
    if (rc != 0)
        return throw_tape_error(ctx, "kv", rc);
    if (outcome == 2) {
        if (json_ptr) free((void *)(intptr_t)json_ptr);
        return JS_ThrowInternalError(ctx, "kv.prefix: recorded failure");
    }
    JSValue arr = JS_ParseJSON(ctx, (const char *)(intptr_t)json_ptr,
                                (size_t)json_len, "<kv.prefix>");
    free((void *)(intptr_t)json_ptr);
    return arr;
}

/* ───────────── module loader ───────────── */

static JSModuleDef *jsb_module_loader(JSContext *ctx, const char *module_name,
                                       void *opaque)
{
    (void)opaque;
    int src_ptr = 0, src_len = 0;
    int rc = _arena_host_module_load(
        (int)(intptr_t)module_name, (int)strlen(module_name),
        &src_ptr, &src_len);
    if (rc != 0) {
        throw_tape_error(ctx, "module", rc);
        return NULL;
    }
    JSValue compiled = JS_Eval(ctx, (const char *)(intptr_t)src_ptr,
                                (size_t)src_len, module_name,
                                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free((void *)(intptr_t)src_ptr);
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

/* ───────────── registration ───────────── */

int arena_install_replay_bindings(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    /* Math.random */
    JSValue math = JS_GetPropertyStr(ctx, global, "Math");
    if (JS_IsObject(math)) {
        JS_SetPropertyStr(ctx, math, "random",
            JS_NewCFunction(ctx, jsb_math_random, "random", 0));
    }
    JS_FreeValue(ctx, math);

    /* Date.now: bind to the original Date constructor object first. */
    JSValue date_ctor = JS_GetPropertyStr(ctx, global, "Date");
    if (JS_IsObject(date_ctor)) {
        JS_SetPropertyStr(ctx, date_ctor, "now",
            JS_NewCFunction(ctx, jsb_date_now, "now", 0));
    }
    JS_FreeValue(ctx, date_ctor);

    /* new Date() with no args: replace the Date constructor entirely
       with a thin JS wrapper that funnels the no-arg case through the
       tape-bound Date.now() and passes everything else to the original.
       Statics (UTC, parse, now, …) and prototype are preserved so
       `instanceof Date` and `Date.UTC(…)` keep working.

       Written in JS rather than C because the prop-copy loop is
       trivial in JS and ugly in C. Pre-freeze eval ⇒ wrapper lives in
       base memory like the rest of the replay surface. */
    static const char date_wrapper_src[] =
        "(() => {\n"
        "  const OrigDate = globalThis.Date;\n"
        "  const D = function(...args) {\n"
        "    if (new.target && args.length === 0) {\n"
        "      return Reflect.construct(OrigDate, [Date.now()], new.target);\n"
        "    }\n"
        "    if (new.target) {\n"
        "      return Reflect.construct(OrigDate, args, new.target);\n"
        "    }\n"
        "    return OrigDate(...args);\n"  /* Date() without new: passthrough */
        "  };\n"
        "  for (const k of Reflect.ownKeys(OrigDate)) {\n"
        "    if (k === 'prototype' || k === 'name' || k === 'length') continue;\n"
        "    try { D[k] = OrigDate[k]; } catch (e) {}\n"
        "  }\n"
        "  Object.defineProperty(D, 'prototype', {\n"
        "    value: OrigDate.prototype, writable: false,\n"
        "    enumerable: false, configurable: false,\n"
        "  });\n"
        "  globalThis.Date = D;\n"
        "})()\n";
    JSValue r = JS_Eval(ctx, date_wrapper_src, sizeof(date_wrapper_src) - 1,
                         "<arena-date-wrapper>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        fprintf(stderr, "Date wrapper install failed: %s\n", s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, global);
        return -1;
    }
    JS_FreeValue(ctx, r);

    /* crypto namespace */
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "getRandomValues",
        JS_NewCFunction(ctx, jsb_crypto_getRandomValues, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, crypto, "randomBytes",
        JS_NewCFunction(ctx, jsb_crypto_randomBytes, "randomBytes", 1));
    JS_SetPropertyStr(ctx, crypto, "randomUUID",
        JS_NewCFunction(ctx, jsb_crypto_randomUUID, "randomUUID", 0));
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

    JS_FreeValue(ctx, global);
    return 0;
}
