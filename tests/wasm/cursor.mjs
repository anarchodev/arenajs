// Browser-side replay cursor module.
//
// Wraps an already-initialised qjs_arena_wasm Module instance and
// exposes the three-verb surface from REPLAY_CURSOR_API.md:
//
//   scanIndex(replay)              — cached list of FUNC_ENTER/EXIT/THROW
//                                    events for the whole replay
//   openCursor(replay, anchor)     — position into the drill stream
//   drillNext(cursor, limit)       — pull up to `limit` drill events
//
// One CursorEngine wraps one long-lived arena and dispatches many
// replays into it; `JS_ResetRequestArena` runs at the top of each
// `arena_run_module` call so per-request state doesn't bleed across
// pages. The scan→drill flip happens from inside host_trace once the
// anchor's bracketing scan ordinal is observed.
//
// Caller responsibility: build the Module, run `arena_init(...)` to
// completion, then hand it in. Reused across scanIndex / drillNext.

const TRACE_OFF = 0, TRACE_SCAN = 1, TRACE_DRILL = 2;
const K_NAME = 0, K_FUNC_ENTER = 1, K_FUNC_EXIT = 2, K_LINE = 3, K_THROW = 4;

export class CursorEngine {
    constructor(Module) {
        this.M = Module;
        this._run     = Module.cwrap("arena_run_module",     "number", ["string","string"]);
        this._setMode = Module.cwrap("arena_set_trace_mode", null,     ["number"]);
        this._scanCache = new WeakMap();
    }

    _decoder() {
        const M = this.M;
        const dec = new TextDecoder();
        return {
            u32: (p)    => M.HEAPU32[p >> 2],
            u16: (p)    => M.HEAPU16[p >> 1],
            str: (p, n) => dec.decode(M.HEAPU8.subarray(p, p + n)),
        };
    }

    _installReplay(replay) {
        // Reset every tape channel's read cursor so the run replays
        // from the start. Mirrors module-smoke's setTapes.
        for (const k of Object.keys(replay.tapes)) {
            replay.tapes[k]._cursor = 0;
        }
        this.M.tapes = replay.tapes;
        this.M.module_sources = replay.module_sources ?? {};
    }

    async scanIndex(replay) {
        const cached = this._scanCache.get(replay);
        if (cached) return cached;

        this._installReplay(replay);
        const r = this._decoder();
        const atomMap = new Map();
        const fileStack = [];
        const records = [];
        let ordinal = 0, depth = 0;

        this.M.host_trace = (kind, ptr) => {
            if (kind === K_NAME) {
                atomMap.set(r.u32(ptr), r.str(ptr + 6, r.u16(ptr + 4)));
                return 0;
            }
            if (kind === K_FUNC_ENTER) {
                const nameAtom = r.u32(ptr);
                const fileAtom = r.u32(ptr + 4);
                const line = r.u32(ptr + 8);
                const name = atomMap.get(nameAtom) ?? `<atom:${nameAtom}>`;
                const file = atomMap.get(fileAtom) ?? `<atom:${fileAtom}>`;
                depth++;
                fileStack.push(file);
                records.push({
                    ordinal: ordinal++, kind: "FUNC_ENTER",
                    name, file, line, depth,
                });
            } else if (kind === K_FUNC_EXIT) {
                const file = fileStack.pop() ?? "";
                records.push({
                    ordinal: ordinal++, kind: "FUNC_EXIT",
                    file, line: 0, depth,
                });
                depth--;
            } else if (kind === K_THROW) {
                const fileAtom = r.u32(ptr);
                const line = r.u32(ptr + 4);
                const mlen = r.u16(ptr + 8);
                records.push({
                    ordinal: ordinal++, kind: "THROW",
                    file: atomMap.get(fileAtom) ?? `<atom:${fileAtom}>`,
                    line, depth,
                    message: r.str(ptr + 10, mlen),
                });
            }
            // LINE events shouldn't fire in scan mode; ignore if any leak.
            return 0;
        };

        this._setMode(TRACE_SCAN);
        this._run(replay.entry.name, replay.entry.src);
        this._setMode(TRACE_OFF);
        this.M.host_trace = null;

        this._scanCache.set(replay, records);
        return records;
    }

    openCursor(replay, anchor) {
        return { replay, anchor, drillEventsAfterAnchor: 0 };
    }

    async drillNext(cursor, limit) {
        if (!Number.isInteger(limit) || limit <= 0)
            throw new Error("drillNext: limit must be a positive integer");

        this._installReplay(cursor.replay);
        const r = this._decoder();
        const atomMap = new Map();
        const fileStack = [];
        const collected = [];
        const anchor = cursor.anchor;
        let scanCounter = 0, depth = 0;
        let state = "SEEKING_ANCHOR";
        let dropRemaining = cursor.drillEventsAfterAnchor;

        this.M.host_trace = (kind, ptr) => {
            if (kind === K_NAME) {
                atomMap.set(r.u32(ptr), r.str(ptr + 6, r.u16(ptr + 4)));
                return 0;
            }

            // Decode + maintain depth/file-stack for all scan-kind events.
            let event;
            let isScan = true;
            if (kind === K_FUNC_ENTER) {
                const nameAtom = r.u32(ptr);
                const fileAtom = r.u32(ptr + 4);
                const line = r.u32(ptr + 8);
                const name = atomMap.get(nameAtom) ?? `<atom:${nameAtom}>`;
                const file = atomMap.get(fileAtom) ?? `<atom:${fileAtom}>`;
                depth++;
                fileStack.push(file);
                event = { kind: "FUNC_ENTER", name, file, line, depth };
            } else if (kind === K_FUNC_EXIT) {
                const file = fileStack.pop() ?? "";
                event = { kind: "FUNC_EXIT", file, line: 0, depth };
                depth--;
            } else if (kind === K_LINE) {
                const fileAtom = r.u32(ptr);
                event = {
                    kind: "LINE",
                    file: atomMap.get(fileAtom) ?? `<atom:${fileAtom}>`,
                    line: r.u32(ptr + 4),
                };
                isScan = false;
            } else if (kind === K_THROW) {
                const fileAtom = r.u32(ptr);
                const line = r.u32(ptr + 4);
                const mlen = r.u16(ptr + 8);
                event = {
                    kind: "THROW",
                    file: atomMap.get(fileAtom) ?? `<atom:${fileAtom}>`,
                    line, depth,
                    message: r.str(ptr + 10, mlen),
                };
            } else {
                return 0;
            }

            if (isScan) {
                event.scanOrdinal = scanCounter;
                scanCounter++;
            } else {
                // LINE — stamp with most recent scan ordinal.
                event.scanOrdinal = Math.max(0, scanCounter - 1);
            }

            switch (state) {
                case "SEEKING_ANCHOR":
                    if (anchor.kind === "scan") {
                        if (isScan && event.scanOrdinal === anchor.ordinal) {
                            this._setMode(TRACE_DRILL);
                            state = "COLLECTING";
                        }
                    } else { // "line"
                        const after = anchor.afterScan ?? 0;
                        if (isScan && event.scanOrdinal >= after) {
                            this._setMode(TRACE_DRILL);
                            state = "SEEKING_LINE";
                        }
                    }
                    return 0;

                case "SEEKING_LINE":
                    if (event.kind === "LINE" &&
                        event.file === anchor.file &&
                        event.line === anchor.line) {
                        state = "COLLECTING";
                    }
                    return 0;

                case "COLLECTING":
                    if (dropRemaining > 0) { dropRemaining--; return 0; }
                    collected.push(event);
                    if (collected.length >= limit) return 1;
                    return 0;
            }
            return 0;
        };

        this._setMode(TRACE_SCAN);
        this._run(cursor.replay.entry.name, cursor.replay.entry.src);
        this._setMode(TRACE_OFF);
        this.M.host_trace = null;

        // If we never filled the buffer, the run finished naturally —
        // no more pages.
        const ended = (collected.length < limit);
        const next = ended ? null : {
            replay: cursor.replay,
            anchor: cursor.anchor,
            drillEventsAfterAnchor:
                cursor.drillEventsAfterAnchor + collected.length,
        };
        return { events: collected, next };
    }
}
