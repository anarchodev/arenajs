cmake_minimum_required(VERSION 3.10)

project(quickjs LANGUAGES C)

include(CheckCCompilerFlag)
include(GNUInstallDirs)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)

macro(xoption OPTION_NAME OPTION_TEXT OPTION_DEFAULT)
    option(${OPTION_NAME} ${OPTION_TEXT} ${OPTION_DEFAULT})
    if(DEFINED ENV{${OPTION_NAME}})
        # Allow setting the option through an environment variable.
        set(${OPTION_NAME} $ENV{${OPTION_NAME}})
    endif()
    if(${OPTION_NAME})
        add_definitions(-D${OPTION_NAME})
    endif()
    message(STATUS "  ${OPTION_NAME}: ${${OPTION_NAME}}")
endmacro()

# note: QJS_ENABLE_TSAN is currently incompatible with the other sanitizers but we
# don't explicitly check for that because who knows what the future will bring?
# QJS_ENABLE_MSAN only works with clang at the time of writing; also not checked
# for the same reason
xoption(BUILD_SHARED_LIBS "Build a shared library" OFF)
# if we ever require CMake 3.21, we can automatically detect this setting by looking at PROJECT_IS_TOP_LEVEL
# similar to how glslang does
xoption(QJS_ENABLE_INSTALL "Enable quickjs installation" ON)
xoption(QJS_BUILD_WERROR "Build with -Werror" OFF)
xoption(QJS_BUILD_EXAMPLES "Build examples" OFF)
# Compile the structural trace emitter into every target (incl. the native
# libqjs). OFF by default: every trace patch site in quickjs.c folds to a
# no-op, so the default native worker pays nothing. ON wires up the emitter
# and the native arena_trace_set_host() sink — see README "Native tracing".
# (The qjs_arena_wasm and arena_trace_native targets force this on for
# themselves regardless of this global option.)
xoption(ARENA_TRACE_ENABLED "Compile the trace emitter into all targets" OFF)
xoption(QJS_BUILD_CLI_STATIC "Build a static qjs executable" OFF)
xoption(QJS_BUILD_CLI_WITH_MIMALLOC "Build the qjs executable with mimalloc" OFF)
xoption(QJS_BUILD_CLI_WITH_STATIC_MIMALLOC "Build the qjs executable with mimalloc (statically linked)" OFF)
xoption(QJS_DISABLE_PARSER "Disable JS source code parser" OFF)
xoption(QJS_ENABLE_ASAN "Enable AddressSanitizer (ASan)" OFF)
xoption(QJS_ENABLE_MSAN "Enable MemorySanitizer (MSan)" OFF)
xoption(QJS_ENABLE_TSAN "Enable ThreadSanitizer (TSan)" OFF)
xoption(QJS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (UBSan)" OFF)
xoption(QJS_ENABLE_GC_STRESS "Force a full GC before every allocation (GC stress test)" OFF)

# Used to properly define JS_LIBC_EXTERN.
add_compile_definitions(QUICKJS_NG_BUILD)

# MINGW doesn't exist in older cmake versions, newer versions don't know
# about CMAKE_COMPILER_IS_MINGW, and there is no unique CMAKE_C_COMPILER_ID
# for mingw-based compilers...
if(MINGW)
    # do nothing
elseif(CMAKE_C_COMPILER MATCHES "mingw")
    set(MINGW TRUE)
else()
    set(MINGW FALSE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(TVOS TRUE)
else()
    set(TVOS FALSE)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    set(WATCHOS TRUE)
else()
    set(WATCHOS FALSE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    set(SUNOS TRUE)
else()
    set(SUNOS FALSE)
endif()

if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE "Release")
endif()

message(STATUS "Building in ${CMAKE_BUILD_TYPE} mode")
message(STATUS "Building with ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} on ${CMAKE_SYSTEM}")

macro(xcheck_add_c_compiler_flag FLAG)
    string(REPLACE "-" "" FLAG_NO_HYPHEN ${FLAG})
    check_c_compiler_flag(${FLAG} COMPILER_SUPPORTS_${FLAG_NO_HYPHEN})
    if(COMPILER_SUPPORTS_${FLAG_NO_HYPHEN})
        add_compile_options(${FLAG})
    endif()
endmacro()

xcheck_add_c_compiler_flag(-Wall)
if(NOT MSVC AND NOT IOS AND NOT TVOS AND NOT WATCHOS)
    if(QJS_BUILD_WERROR)
        xcheck_add_c_compiler_flag(-Werror)
    endif()
    xcheck_add_c_compiler_flag(-Wextra)
endif()
xcheck_add_c_compiler_flag(-Wformat=2)
xcheck_add_c_compiler_flag(-Wno-implicit-fallthrough)
xcheck_add_c_compiler_flag(-Wno-sign-compare)
xcheck_add_c_compiler_flag(-Wno-missing-field-initializers)
xcheck_add_c_compiler_flag(-Wno-unused-parameter)
xcheck_add_c_compiler_flag(-Wno-unused-but-set-variable)
xcheck_add_c_compiler_flag(-Wno-unused-result)
xcheck_add_c_compiler_flag(-Wno-stringop-truncation)
xcheck_add_c_compiler_flag(-Wno-array-bounds)
if(NOT SUNOS)
xcheck_add_c_compiler_flag(-funsigned-char)
endif()

# Clang on Windows without MSVC command line fails because the codebase uses
# functions like strcpy over strcpy_s
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND WIN32 AND NOT MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(_CRT_NONSTDC_NO_DEPRECATE)
endif()

# ClangCL is command line compatible with MSVC, so 'MSVC' is set.
if(MSVC)
    xcheck_add_c_compiler_flag(-Wno-unsafe-buffer-usage)
    xcheck_add_c_compiler_flag(-Wno-sign-conversion)
    xcheck_add_c_compiler_flag(-Wno-nonportable-system-include-path)
    xcheck_add_c_compiler_flag(-Wno-implicit-int-conversion)
    xcheck_add_c_compiler_flag(-Wno-shorten-64-to-32)
    xcheck_add_c_compiler_flag(-Wno-reserved-macro-identifier)
    xcheck_add_c_compiler_flag(-Wno-reserved-identifier)
    xcheck_add_c_compiler_flag(-Wdeprecated-declarations)
    xcheck_add_c_compiler_flag(/experimental:c11atomics)
    xcheck_add_c_compiler_flag(/wd4018) # -Wno-sign-conversion
    xcheck_add_c_compiler_flag(/wd4061) # -Wno-implicit-fallthrough
    xcheck_add_c_compiler_flag(/wd4100) # -Wno-unused-parameter
    xcheck_add_c_compiler_flag(/wd4200) # -Wno-zero-length-array
    xcheck_add_c_compiler_flag(/wd4242) # -Wno-shorten-64-to-32
    xcheck_add_c_compiler_flag(/wd4244) # -Wno-shorten-64-to-32
    xcheck_add_c_compiler_flag(/wd4245) # -Wno-sign-compare
    xcheck_add_c_compiler_flag(/wd4267) # -Wno-shorten-64-to-32
    xcheck_add_c_compiler_flag(/wd4388) # -Wno-sign-compare
    xcheck_add_c_compiler_flag(/wd4389) # -Wno-sign-compare
    xcheck_add_c_compiler_flag(/wd4456) # Hides previous local declaration
    xcheck_add_c_compiler_flag(/wd4457) # Hides function parameter
    xcheck_add_c_compiler_flag(/wd4710) # Function not inlined
    xcheck_add_c_compiler_flag(/wd4711) # Function was inlined
    xcheck_add_c_compiler_flag(/wd4820) # Padding added after construct
    xcheck_add_c_compiler_flag(/wd4996) # -Wdeprecated-declarations
    xcheck_add_c_compiler_flag(/wd5045) # Compiler will insert Spectre mitigation for memory load if /Qspectre switch specified
endif()

# Set a 8MB default stack size on Windows.
# It defaults to 1MB on MSVC, which is the same as our current JS stack size,
# so it will overflow and crash otherwise.
# On MinGW it defaults to 2MB.
if(WIN32)
    if(MSVC)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:8388608")
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT MINGW)
        # Clang frontend (clang.exe) targeting the MSVC ABI (using lld-link)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Xlinker /STACK:8388608")
    else()
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--stack,8388608")
    endif()
endif()

# MacOS and GCC 11 or later need -Wno-maybe-uninitialized
# https://github.com/quickjs-ng/quickjs/issues/453
if(APPLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 11)
    xcheck_add_c_compiler_flag(-Wno-maybe-uninitialized)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    add_compile_definitions(
        _WASI_EMULATED_PROCESS_CLOCKS
        _WASI_EMULATED_SIGNAL
    )
    add_link_options(
        -lwasi-emulated-process-clocks
        -lwasi-emulated-signal
    )
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    xcheck_add_c_compiler_flag(/Od)
    xcheck_add_c_compiler_flag(-O0)
    xcheck_add_c_compiler_flag(-ggdb)
    xcheck_add_c_compiler_flag(-fno-omit-frame-pointer)
endif()

if(BUILD_SHARED_LIBS)
    message(STATUS "Building a shared library")
endif()

if(QJS_ENABLE_ASAN)
message(STATUS "Building with ASan")
add_compile_options(
    -fsanitize=address
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
add_link_options(
    -fsanitize=address
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
endif()

if(QJS_ENABLE_MSAN)
message(STATUS "Building with MSan")
add_compile_options(
    -fsanitize=memory
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
add_link_options(
    -fsanitize=memory
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
endif()

if(QJS_ENABLE_TSAN)
message(STATUS "Building with TSan")
add_compile_options(
    -fsanitize=thread
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
add_link_options(
    -fsanitize=thread
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
endif()

if(QJS_ENABLE_UBSAN)
message(STATUS "Building with UBSan")
add_compile_options(
    -fsanitize=undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
add_link_options(
    -fsanitize=undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
)
endif()

if(QJS_ENABLE_GC_STRESS)
message(STATUS "Building with GC stress (FORCE_GC_AT_MALLOC)")
add_compile_definitions(FORCE_GC_AT_MALLOC)
endif()


# QuickJS library
#

xoption(QJS_BUILD_LIBC "Build standard library modules as part of the library" OFF)
macro(add_qjs_libc_if_needed target)
    if(NOT QJS_BUILD_LIBC)
        target_link_libraries(${target} PRIVATE qjs-libc)
    endif()
endmacro()
macro(add_static_if_needed target)
    if(QJS_BUILD_CLI_STATIC OR MINGW)
        target_link_options(${target} PRIVATE -static)
        if(MINGW)
            target_link_options(${target} PRIVATE -static-libgcc)
        endif()
    endif()
endmacro()

set(qjs_sources
    dtoa.c
    libregexp.c
    libunicode.c
    qjs-arena.c
    qjs-dlmalloc.c
    qjs-arena-trace.c
    quickjs.c
)

if(QJS_BUILD_LIBC)
    list(APPEND qjs_sources quickjs-libc.c)
    # The definition must be added to the entire project.
    add_compile_definitions(QJS_BUILD_LIBC)
endif()
list(APPEND qjs_defines _GNU_SOURCE)
if(WIN32)
    # NB: Windows 7 is EOL and we are only supporting in so far as it doesn't interfere with progress.
    list(APPEND qjs_defines WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()
if(TVOS)
    list(APPEND qjs_defines _TVOS)
endif()
if(WATCHOS)
    list(APPEND qjs_defines _WATCHOS)
endif()
list(APPEND qjs_libs ${CMAKE_DL_LIBS})
find_package(Threads)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    list(APPEND qjs_libs ${CMAKE_THREAD_LIBS_INIT})
endif()

# try to find libm
find_library(M_LIBRARIES m)
if(M_LIBRARIES OR CMAKE_C_COMPILER_ID STREQUAL "TinyCC")
    list(APPEND qjs_libs m)
endif()

add_library(qjs ${qjs_sources})
target_compile_definitions(qjs PRIVATE ${qjs_defines})
target_include_directories(qjs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(qjs PUBLIC ${qjs_libs})

# Pass a compiler definition so that Windows gets its declspec's right.
get_target_property(QJS_LIB_TYPE qjs TYPE)
if(QJS_LIB_TYPE STREQUAL "SHARED_LIBRARY")
    target_compile_definitions(qjs
        PRIVATE BUILDING_QJS_SHARED
        PUBLIC  USING_QJS_SHARED
    )
endif()

# An interface library for modules.
add_library(qjs_module_lib INTERFACE)
target_include_directories(qjs_module_lib INTERFACE
    $<TARGET_PROPERTY:qjs,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(qjs_module_lib INTERFACE
    QUICKJS_NG_MODULE_BUILD
    $<TARGET_PROPERTY:qjs,INTERFACE_COMPILE_DEFINITIONS>
)
if(WIN32)
    # Since Windows cannot resolve symbols at load time, we need to
    # explicitly link it to qjs.
    target_link_libraries(qjs_module_lib INTERFACE
        qjs
    )
endif()

if(NOT QJS_BUILD_LIBC AND NOT DEFINED ENV{QJS_ARENA_WASM_ONLY})
    # Dev-tree-only: quickjs-libc.c is not in the package (nothing the wasm
    # target links needs it).
    add_library(qjs-libc STATIC quickjs-libc.c)
    target_compile_definitions(qjs-libc PRIVATE ${qjs_defines})
    target_link_libraries(qjs-libc PRIVATE ${qjs_libs} qjs)
endif()

if(EMSCRIPTEN)
    add_executable(qjs_wasm ${qjs_sources})
    target_link_options(qjs_wasm PRIVATE
        # in emscripten 3.x, this will be set to 16k which is too small for quickjs. #write sth. to force github rebuild
        -sSTACK_SIZE=2097152 # let it be 2m = 2 * 1024 * 1024 = 2097152, otherwise, stack overflow may be occured at bootstrap
        -sNO_INVOKE_RUN
        -sNO_EXIT_RUNTIME
        -sMODULARIZE # do not mess the global
        -sEXPORT_ES6 # export js file to morden es module
        -sEXPORT_NAME=getQuickJs # give a name
        -sTEXTDECODER=1 # it will be 2 if we use -Oz, and that will cause js -> c string convertion fail
        -sNO_DEFAULT_TO_CXX # this project is pure c project, no need for c plus plus handle
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
    )
    target_compile_definitions(qjs_wasm PRIVATE ${qjs_defines})
    target_link_libraries(qjs_wasm PRIVATE m)

    # arenajs reactor: minimal browser-facing module that exports
    # arena_init/arena_run/arena_destroy. Consumed by rove's replay UI.
    # Tape host imports and trace-event emitter land in follow-ons.
    set(ARENA_WASM_EXPORTS "_arena_init,_arena_init_open,_arena_eval_base,_arena_freeze,_arena_run,_arena_run_module,_arena_set_trace_mode,_arena_set_request_mode,_arena_snapshot_here,_arena_set_random_seed,_arena_set_date_now,_arena_oom_hit,_arena_oom_requested,_arena_oom_used,_arena_oom_limit,_arena_destroy,_malloc,_free")
    if(DEFINED ENV{ROVE_ARENA_LIB})
        # The embedder archive's cwrap-reachable exports ride the same list.
        string(APPEND ARENA_WASM_EXPORTS ",_rove_arena_set_budget_ms")
    endif()
    add_executable(qjs_arena_wasm
        ${qjs_sources}
        qjs-arena-reactor.c
        qjs-arena-replay-bindings.c
    )
    target_link_options(qjs_arena_wasm PRIVATE
        -sSTACK_SIZE=2097152
        -sNO_INVOKE_RUN
        -sNO_EXIT_RUNTIME
        -sMODULARIZE
        -sEXPORT_ES6
        -sEXPORT_NAME=getArenaJs
        -sTEXTDECODER=1
        -sNO_DEFAULT_TO_CXX
        # Allow linear memory to grow past the 16 MB INITIAL default.
        # The request arena's snapshot_here path can produce many MB
        # of transient JSValues for dense-cadence × deep-stack traces,
        # so embedders need to size the arena generously and have
        # WASM memory follow.
        -sALLOW_MEMORY_GROWTH=1
        # HEAPU8/HEAPU16/HEAPU32 exposed so the host can decode binary
        # trace payloads written by the WASM into linear memory.
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,HEAPU16,HEAPU32
        "-sEXPORTED_FUNCTIONS=${ARENA_WASM_EXPORTS}"
    )
    # Turn on the trace emitter for this target. Default 0 means the
    # native qjs library (used by the rove worker) has zero trace
    # overhead — every patch site in quickjs.c folds to a no-op.
    target_compile_definitions(qjs_arena_wasm PRIVATE ${qjs_defines} ARENA_TRACE_ENABLED=1)
    target_link_libraries(qjs_arena_wasm PRIVATE m)
    # Optional embedder archive (rove's compiled binding — wasm32-emscripten
    # static lib built by the embedder's own build). When set, the reactor's
    # default base-setup installs it into every context (ROVE_ARENA).
    if(DEFINED ENV{ROVE_ARENA_LIB})
        target_compile_definitions(qjs_arena_wasm PRIVATE ROVE_ARENA=1)
        target_link_libraries(qjs_arena_wasm PRIVATE $ENV{ROVE_ARENA_LIB})
    endif()
endif()


# QuickJS bytecode compiler
#

# ── dev-tree-only targets ──
#
# Everything below references sources the PACKAGED tree (the embedder's
# fetched copy, build.zig.zon .paths) does not ship: the CLI, tests,
# generators, examples. An embedder building qjs_arena_wasm from the package
# sets QJS_ARENA_WASM_ONLY=1 (rove's build_wasm_arena.sh does) and gets the
# libraries + wasm targets alone.
if(NOT DEFINED ENV{QJS_ARENA_WASM_ONLY})

add_executable(qjsc
    qjsc.c
)
add_qjs_libc_if_needed(qjsc)
add_static_if_needed(qjsc)
target_compile_definitions(qjsc PRIVATE ${qjs_defines})
target_link_libraries(qjsc PRIVATE qjs)


# QuickJS CLI
#

add_executable(qjs_exe
    gen/repl.c
    gen/standalone.c
    qjs.c
)
add_qjs_libc_if_needed(qjs_exe)
add_static_if_needed(qjs_exe)
set_target_properties(qjs_exe PROPERTIES
    OUTPUT_NAME "qjs"
    PDB_NAME "qjs_cli"
    COMPILE_PDB_NAME "qjs_cli"
)
target_compile_definitions(qjs_exe PRIVATE ${qjs_defines})
target_link_libraries(qjs_exe PRIVATE qjs)
if (NOT WIN32)
    set_target_properties(qjs_exe PROPERTIES ENABLE_EXPORTS TRUE)
endif()

# WASI Reactor
#

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    option(QJS_WASI_REACTOR "Build WASI reactor (exports library functions, no _start)" OFF)
    if(QJS_WASI_REACTOR)
        add_executable(qjs_wasi
            quickjs-libc.c
            qjs-wasi-reactor.c
        )
        set_target_properties(qjs_wasi PROPERTIES
            OUTPUT_NAME "qjs"
            SUFFIX ".wasm"
        )
        target_compile_definitions(qjs_wasi PRIVATE ${qjs_defines})
        target_link_libraries(qjs_wasi qjs)
        target_link_options(qjs_wasi PRIVATE
            -mexec-model=reactor
            # Export all symbols with default visibility (JS_EXTERN functions)
            -Wl,--export-dynamic
            # Memory management (libc symbols need explicit export)
            -Wl,--export=malloc
            -Wl,--export=free
            -Wl,--export=realloc
            -Wl,--export=calloc
        )
    endif()
endif()

if(QJS_BUILD_CLI_WITH_MIMALLOC OR QJS_BUILD_CLI_WITH_STATIC_MIMALLOC)
    find_package(mimalloc REQUIRED)
    target_compile_definitions(qjs_exe PRIVATE QJS_USE_MIMALLOC)
    # Upstream mimalloc doesn't provide a way to know if both libraries are supported.
    if(QJS_BUILD_CLI_WITH_STATIC_MIMALLOC)
        target_link_libraries(qjs_exe PRIVATE mimalloc-static)
    else()
        target_link_libraries(qjs_exe PRIVATE mimalloc)
    endif()
endif()

# Test262 runner
#

if(NOT EMSCRIPTEN)
    add_executable(run-test262
        run-test262.c
    )
    add_qjs_libc_if_needed(run-test262)
    target_compile_definitions(run-test262 PRIVATE ${qjs_defines})
    target_link_libraries(run-test262 PRIVATE qjs)
endif()

# Interrupt test
#

add_executable(api-test
    api-test.c
)
target_compile_definitions(api-test PRIVATE ${qjs_defines})
target_link_libraries(api-test PRIVATE qjs)

# Arena harnesses: smoke (basic two-request lifecycle), stress
# (kitchen-sink of 38 JS workloads), bench (per-request reset speed),
# test262 (walks the test262 corpus under the thermometer asserting
# zero base-byte mutations per request), atomgrow (sweeps the snapshot's
# atom count to catch base tables being grown post-freeze). All link against the same
# libqjs as api-test; they read test262 files at runtime relative to
# the build's source dir for arena-test262.

foreach(arena_target IN ITEMS arena-smoke arena-stress arena-bench arena-test262 arena-coexist arena-interrupt arena-basemap arena-atomgrow arena-baseobj arena-baseclass)
    add_executable(${arena_target} ${arena_target}.c)
    target_compile_definitions(${arena_target} PRIVATE ${qjs_defines})
    target_link_libraries(${arena_target} PRIVATE qjs)
endforeach()

add_executable(lre-test
    lre-test.c
    libregexp.c
    libunicode.c
)
target_compile_definitions(lre-test PRIVATE ${qjs_defines})

# Unicode generator
#

# Note: do not link libunicode.c here, it needs the generated libunicode-table.h,
# which would make it impossible to regenerate the table from scratch.
add_executable(unicode_gen EXCLUDE_FROM_ALL
    unicode_gen.c
)
target_compile_definitions(unicode_gen PRIVATE ${qjs_defines})

add_executable(function_source
    gen/function_source.c
)
add_qjs_libc_if_needed(function_source)
target_include_directories(function_source PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(function_source PRIVATE ${qjs_defines})
target_link_libraries(function_source PRIVATE qjs)

# Examples
#

if(QJS_BUILD_EXAMPLES)
    if(WIN32 AND NOT (QJS_LIB_TYPE STREQUAL "SHARED_LIBRARY"))
        message(AUTHOR_WARNING
            "Binary modules used with static qjs on Windows. There might be\n"
            "runtime errors when the module is used due to two copies of qjs\n"
            "in memory.\n"
            "Please use \"-DBUILD_SHARED_LIBS=true\" if possible."
        )
    endif()

    add_executable(hello
        gen/hello.c
    )
    add_qjs_libc_if_needed(hello)
    target_include_directories(hello PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hello PRIVATE ${qjs_defines})
    target_link_libraries(hello PRIVATE qjs)

    add_executable(hello_module
        gen/hello_module.c
    )
    add_qjs_libc_if_needed(hello_module)
    target_include_directories(hello_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hello_module PRIVATE ${qjs_defines})
    target_link_libraries(hello_module PRIVATE qjs)

    # Native trace example. The trace emitter is OFF in libqjs (default
    # ARENA_TRACE_ENABLED=0 → every patch site in quickjs.c folds to a
    # no-op), so this can't link libqjs — it recompiles the core sources +
    # reactor + replay bindings with ARENA_TRACE_ENABLED=1, exactly like
    # the qjs_arena_wasm target but with the C host-callback sink instead
    # of Module.host_trace. Native-only: no emscripten host here.
    if(NOT EMSCRIPTEN)
        # Native reactor + host-binding sources, shared by the examples
        # below. Compiled with the trace emitter on so the trace sink is
        # available; the replay sink is unconditional.
        set(arena_native_host_sources
            ${qjs_sources}
            qjs-arena-reactor.c
            qjs-arena-replay-bindings.c
        )

        # Output side: drive a script and decode its trace events.
        add_executable(arena_trace_native
            ${arena_native_host_sources}
            examples/arena_trace_native.c
        )
        target_include_directories(arena_trace_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(arena_trace_native PRIVATE ${qjs_defines} ARENA_TRACE_ENABLED=1)
        target_link_libraries(arena_trace_native PRIVATE ${qjs_libs})

        # Input side: replay a recorded request by serving its tapes
        # (module source + kv reads) through a native arena_replay_host.
        add_executable(arena_replay_native
            ${arena_native_host_sources}
            examples/arena_replay_native.c
        )
        target_include_directories(arena_replay_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(arena_replay_native PRIVATE ${qjs_defines} ARENA_TRACE_ENABLED=1)
        target_link_libraries(arena_replay_native PRIVATE ${qjs_libs})

        # Multi-instance contract test: two reactors on one thread,
        # runs nested across them (qjs-arena-reactor.h). Exits nonzero
        # on any isolation failure.
        add_executable(arena_multi_reactor
            ${arena_native_host_sources}
            examples/arena_multi_reactor.c
        )
        target_include_directories(arena_multi_reactor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(arena_multi_reactor PRIVATE ${qjs_defines} ARENA_TRACE_ENABLED=1)
        target_link_libraries(arena_multi_reactor PRIVATE ${qjs_libs})
    endif()

    add_library(fib MODULE examples/fib.c)
    set_target_properties(fib PROPERTIES
        PREFIX ""
    )
    target_link_libraries(fib PRIVATE qjs_module_lib)
    if(APPLE)
        target_link_options(fib PRIVATE -undefined dynamic_lookup)
    endif()

    add_library(point MODULE examples/point.c)
    set_target_properties(point PROPERTIES
        PREFIX ""
    )
    target_link_libraries(point PRIVATE qjs_module_lib)
    if(APPLE)
        target_link_options(point PRIVATE -undefined dynamic_lookup)
    endif()

    add_executable(test_fib
        gen/test_fib.c
    )
    add_qjs_libc_if_needed(test_fib)
    target_include_directories(test_fib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(test_fib PRIVATE ${qjs_defines})
    target_link_libraries(test_fib PRIVATE qjs)
    # Loads fib.so which depends on symbols from libqjs.
    set_target_properties(test_fib PROPERTIES ENABLE_EXPORTS TRUE)
endif()

# Install target
#

if (QJS_ENABLE_INSTALL)
    file(STRINGS quickjs.h quickjs_h REGEX QJS_VERSION)
    string(REGEX MATCH "QJS_VERSION_MAJOR ([0-9]*)" _ "${quickjs_h}")
    set(QJS_VERSION_MAJOR ${CMAKE_MATCH_1})
    string(REGEX MATCH "QJS_VERSION_MINOR ([0-9]*)" _ "${quickjs_h}")
    set(QJS_VERSION_MINOR ${CMAKE_MATCH_1})
    string(REGEX MATCH "QJS_VERSION_PATCH ([0-9]*)" _ "${quickjs_h}")
    set(QJS_VERSION_PATCH ${CMAKE_MATCH_1})
    set_target_properties(qjs PROPERTIES
        VERSION ${QJS_VERSION_MAJOR}.${QJS_VERSION_MINOR}.${QJS_VERSION_PATCH}
        SOVERSION ${QJS_VERSION_MAJOR}
    )
    install(FILES quickjs.h qjs-arena.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(QJS_BUILD_LIBC)
        install(FILES quickjs-libc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()
    if(NOT IOS AND NOT TVOS AND NOT WATCHOS)
        install(TARGETS qjs_exe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(TARGETS qjsc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES qjs.man DESTINATION ${CMAKE_INSTALL_DATADIR}/man/man1 RENAME qjs.1)
        install(FILES qjsc.man DESTINATION ${CMAKE_INSTALL_DATADIR}/man/man1 RENAME qjsc.1)
    endif()
    install(TARGETS qjs EXPORT qjsConfig
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT qjsConfig DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qjs)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(DIRECTORY examples DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

endif() # QJS_ARENA_WASM_ONLY
