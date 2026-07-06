//! Build for arenajs — a quickjs-ng fork with a dual bump arena (base
//! + per-request) whose per-request reset collapses to a single cursor
//! write. Exposes a static library `arenajs` (the quickjs runtime
//! compiled with the arena allocator) for consumption as a Zig package:
//!
//! ```zig
//! const dep = b.dependency("arenajs", .{ .target = target, .optimize = optimize });
//! my_mod.addIncludePath(dep.path("."));   // quickjs.h, qjs-arena.h
//! my_mod.linkLibrary(dep.artifact("arenajs"));
//! ```
//!
//! Two artifacts are installed:
//!   - `arenajs`        — the worker engine: the 5 runtime TUs, trace OFF, no
//!                        reactor/replay host (zero per-opcode trace cost).
//!   - `arenajs-replay` — the native replay/simulation engine: the same TUs +
//!                        the reactor + replay-bindings + trace sink, compiled
//!                        with ARENA_TRACE_ENABLED=1 (mirrors the browser
//!                        `qjs_arena_wasm` CMake target). A native embedder
//!                        (`arena_init`/`arena_run_module`/`arena_replay_set_host`
//!                        /`arena_trace_set_host`) links THIS one instead.
//! Never link both into one binary (duplicate quickjs symbols) — pick per use.
//!
//! The C flags below MUST match how consumers historically compiled
//! these sources — the arena base-snapshot freeze is sensitive to build
//! settings, so changing them can corrupt the frozen snapshot.

const std = @import("std");

// The six translation units that make up the runtime. The remaining
// root `.c` files are tests/benches/examples and are not part of the lib.
const sources = [_][]const u8{
    "quickjs.c",
    "qjs-arena.c",
    "qjs-dlmalloc.c",
    "libregexp.c",
    "libunicode.c",
    "dtoa.c",
};

// `arenajs-replay` = the runtime TUs + the native host surface (trace emitter +
// reactor + replay tape responders). Mirrors the browser `qjs_arena_wasm`
// CMake target's source set.
const replay_sources = sources ++ [_][]const u8{
    "qjs-arena-trace.c",
    "qjs-arena-reactor.c",
    "qjs-arena-replay-bindings.c",
};

const cflags = [_][]const u8{
    "-std=c11",
    "-D_GNU_SOURCE",
    "-DQUICKJS_NG_BUILD",
    "-Wno-implicit-fallthrough",
    "-Wno-sign-compare",
    "-Wno-array-bounds",
    "-Wno-unused-parameter",
    "-Wno-unused-but-set-variable",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-fno-sanitize=undefined",
};

// The replay engine wires the trace patch sites in quickjs.c; the worker engine
// leaves them folded out (ARENA_TRACE_ENABLED defaults to 0). The replay engine
// also opts out of the thermometer (ARENA_NO_THERM): the page-dirtiness profiler
// is a worker-side dev tool, never used by replay/simulation embedders (the
// rewind CLI), and dropping it removes the POSIX mmap/signal dependency so the
// replay engine cross-compiles to musl and Windows.
const replay_cflags = cflags ++ [_][]const u8{ "-DARENA_TRACE_ENABLED=1", "-DARENA_NO_THERM=1" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    b.installArtifact(arenaLib(b, target, optimize, "arenajs", &sources, &cflags));
    b.installArtifact(arenaLib(b, target, optimize, "arenajs-replay", &replay_sources, &replay_cflags));
}

fn arenaLib(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    name: []const u8,
    files: []const []const u8,
    flags: []const []const u8,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    mod.linkSystemLibrary("m", .{});
    mod.linkSystemLibrary("pthread", .{});
    mod.addIncludePath(b.path("."));
    mod.addCSourceFiles(.{ .files = files, .flags = flags });
    return b.addLibrary(.{ .linkage = .static, .name = name, .root_module = mod });
}
