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
//! The C flags below MUST match how consumers historically compiled
//! these sources — the arena base-snapshot freeze is sensitive to build
//! settings, so changing them can corrupt the frozen snapshot.

const std = @import("std");

// The five translation units that make up the runtime. The remaining
// root `.c` files are tests/benches/examples and are not part of the lib.
const sources = [_][]const u8{
    "quickjs.c",
    "qjs-arena.c",
    "libregexp.c",
    "libunicode.c",
    "dtoa.c",
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

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    mod.linkSystemLibrary("m", .{});
    mod.linkSystemLibrary("pthread", .{});
    mod.addIncludePath(b.path("."));
    mod.addCSourceFiles(.{ .files = &sources, .flags = &cflags });

    const lib = b.addLibrary(.{
        .linkage = .static,
        .name = "arenajs",
        .root_module = mod,
    });
    b.installArtifact(lib);
}
