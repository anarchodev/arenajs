// Browser-side replay cursor module.
//
// Wraps an already-initialised qjs_arena_wasm Module instance and
// exposes the cursor surface from REPLAY_CURSOR_API.md:
//
//   scanIndex(replay)              — cached list of FUNC_ENTER/EXIT/THROW
//                                    events for the whole replay (cheap)
//   materialise(replay, opts)      — one-pass drill: events array,
//                                    stackSnapshots, matchingExit,
//                                    lineIndex, scanOrdinalToEventIdx
//   openCursor(replay, anchor)     — position into the drill stream
//   drillNext(cursor, limit)       — slice the next `limit` events from
//                                    the materialised data
//
// One CursorEngine wraps one long-lived arena and dispatches many
// replays into it; `JS_ResetRequestArena` runs at the top of each
// `arena_run_module` call so per-request state doesn't bleed across
// replays. `drillNext` is a pure slice over `materialise()` output —
// no per-page replay.

const TRACE_OFF = 0, TRACE_SCAN = 1, TRACE_DRILL = 2;
const K_NAME = 0, K_FUNC_ENTER = 1, K_FUNC_EXIT = 2, K_LINE = 3, K_THROW = 4;

const DEFAULT_STACK_SNAPSHOT_STEP = 64;

export class CursorEngine {
    constructor(Module) {
        this.M = Module;
        this._run      = Module.cwrap("arena_run_module",     "number", ["string","string"]);
        this._setMode  = Module.cwrap("arena_set_trace_mode", null,     ["number"]);
        this._snapshot = Module.cwrap("arena_snapshot_here",  "number", []);
        this._scanCache = new WeakMap();
        this._matCache  = new WeakMap();
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
            return 0;
        };

        this._setMode(TRACE_SCAN);
        this._run(replay.entry.name, replay.entry.src);
        this._setMode(TRACE_OFF);
        this.M.host_trace = null;

        this._scanCache.set(replay, records);
        return records;
    }

    async materialise(replay, opts = {}) {
        const cached = this._matCache.get(replay);
        if (cached) return cached;

        const stackSnapshotStep = opts.stackSnapshotStep ?? DEFAULT_STACK_SNAPSHOT_STEP;
        const varSnapshotStep   = opts.snapshotStep ?? 0;

        this._installReplay(replay);
        const r = this._decoder();

        const atomMap = new Map();
        const fileStack = [];
        const events = [];
        const matchingExitArr = [];      // length tracks events.length
        const scanOrdinalToEventIdx = [];
        const enterIdxStack = [];        // stack of FUNC_ENTER event indices
        const liveStack = [];            // running call frames (mutated)
        const stackSnapshots = [];
        const lineIndex = new Map();     // "file:line" → number[] (packed later)
        const varSnapshots = [];

        let scanCounter = 0;
        let depth = 0;
        let pendingSnapshotJson = null;

        // host_state fires synchronously from arena_snapshot_here(),
        // delivering the JSON payload emit_state built. Only installed
        // if the caller asked for varSnapshots — otherwise per-event
        // cost stays at zero.
        if (varSnapshotStep > 0) {
            this.M.host_state = (ptr, len) => {
                pendingSnapshotJson = r.str(ptr, len);
            };
        }

        this.M.host_trace = (kind, ptr) => {
            if (kind === K_NAME) {
                atomMap.set(r.u32(ptr), r.str(ptr + 6, r.u16(ptr + 4)));
                return 0;
            }

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
                event.scanOrdinal = Math.max(0, scanCounter - 1);
            }

            const eventIdx = events.length;
            events.push(event);
            matchingExitArr.push(-1);

            // Stack maintenance + matchingExit pairing.
            if (event.kind === "FUNC_ENTER") {
                liveStack.push({
                    name: event.name, file: event.file,
                    line: event.line, depth: event.depth,
                });
                enterIdxStack.push(eventIdx);
            } else if (event.kind === "FUNC_EXIT") {
                liveStack.pop();
                const enterIdx = enterIdxStack.pop();
                if (enterIdx !== undefined) {
                    matchingExitArr[enterIdx] = eventIdx;
                    matchingExitArr[eventIdx] = enterIdx;
                }
            } else if (event.kind === "LINE" && liveStack.length > 0) {
                liveStack[liveStack.length - 1].line = event.line;
            }

            if (isScan) scanOrdinalToEventIdx.push(eventIdx);

            // Sparse stack snapshot every stackSnapshotStep events.
            if (stackSnapshotStep > 0 && (eventIdx % stackSnapshotStep) === 0) {
                stackSnapshots.push(liveStack.map(f => ({ ...f })));
            }

            // Variable snapshot every varSnapshotStep events. host_state
            // delivers the JSON synchronously from inside arena_snapshot_here.
            if (varSnapshotStep > 0 && (eventIdx % varSnapshotStep) === 0) {
                pendingSnapshotJson = null;
                this._snapshot();
                if (pendingSnapshotJson !== null) {
                    let frames;
                    try { frames = JSON.parse(pendingSnapshotJson); }
                    catch { frames = []; }
                    varSnapshots.push({ eventOrdinal: eventIdx, frames });
                    pendingSnapshotJson = null;
                }
            }

            // lineIndex covers any event with a meaningful (file, line)
            // — UI can filter by kind on read.
            if (event.kind !== "FUNC_EXIT") {
                const key = `${event.file}:${event.line}`;
                let arr = lineIndex.get(key);
                if (!arr) { arr = []; lineIndex.set(key, arr); }
                arr.push(eventIdx);
            }

            return 0;
        };

        this._setMode(TRACE_DRILL);
        this._run(replay.entry.name, replay.entry.src);
        this._setMode(TRACE_OFF);
        this.M.host_trace = null;
        if (varSnapshotStep > 0) this.M.host_state = null;

        // Pack indices into Int32Arrays. Map keys keep the JS string
        // keys for ergonomics; values are typed.
        const packedLineIndex = new Map();
        for (const [k, v] of lineIndex) packedLineIndex.set(k, Int32Array.from(v));

        const result = {
            replay,
            events,
            matchingExit: Int32Array.from(matchingExitArr),
            stackSnapshots,
            stackSnapshotStep,
            lineIndex: packedLineIndex,
            scanOrdinalToEventIdx: Int32Array.from(scanOrdinalToEventIdx),
            varSnapshots: varSnapshotStep > 0 ? varSnapshots : undefined,
            varSnapshotStep: varSnapshotStep > 0 ? varSnapshotStep : undefined,
            inspectCache: new Map(),
        };
        this._matCache.set(replay, result);
        return result;
    }

    openCursor(replay, anchor) {
        return { replay, anchor, drillEventsAfterAnchor: 0 };
    }

    async drillNext(cursor, limit) {
        if (!Number.isInteger(limit) || limit <= 0)
            throw new Error("drillNext: limit must be a positive integer");

        const mat = await this.materialise(cursor.replay);
        const anchorIdx = this._anchorToEventIdx(mat, cursor.anchor);
        if (anchorIdx < 0) {
            // Anchor not found in this replay — return empty page,
            // signal end (no more pages to fetch for an unmatched anchor).
            return { events: [], next: null };
        }

        const start = anchorIdx + 1 + cursor.drillEventsAfterAnchor;
        const end = Math.min(start + limit, mat.events.length);
        const events = mat.events.slice(start, end);

        const next = (end < mat.events.length) ? {
            replay: cursor.replay,
            anchor: cursor.anchor,
            drillEventsAfterAnchor:
                cursor.drillEventsAfterAnchor + events.length,
        } : null;

        return { events, next };
    }

    _anchorToEventIdx(mat, anchor) {
        if (anchor.kind === "scan") {
            const ord = anchor.ordinal;
            if (ord < 0 || ord >= mat.scanOrdinalToEventIdx.length) return -1;
            return mat.scanOrdinalToEventIdx[ord];
        }
        // line anchor
        const after = anchor.afterScan ?? 0;
        const afterEventIdx = (after < mat.scanOrdinalToEventIdx.length)
            ? mat.scanOrdinalToEventIdx[after]
            : -1;
        const key = `${anchor.file}:${anchor.line}`;
        const indices = mat.lineIndex.get(key);
        if (!indices) return -1;
        for (let i = 0; i < indices.length; i++) {
            const idx = indices[i];
            if (idx > afterEventIdx && mat.events[idx].kind === "LINE") {
                return idx;
            }
        }
        return -1;
    }
}
