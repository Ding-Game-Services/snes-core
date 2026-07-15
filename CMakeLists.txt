# ── CMakeLists.txt ────────────────────────────────────────────────────────────
# D!NG SNES core (snes-core)
#
# Two build outputs, matching every other Ding core repo:
#   1. Native shared lib (.dll on Windows / .so on Linux/Steam Deck/ROG Ally)
#      for Hydra, loaded like any other libretro-style core.
#   2. WASM module (built with Emscripten) for the browser emulator frontend.
#
# Usage — native (Windows, MinGW Makefiles; Ninja isn't available there):
#   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Usage — native (Linux/Steam Deck):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Usage — WASM (requires emsdk activated in your shell):
#   emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.20)
project(ding_snes_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# x86_64 only — matches every other Ding core (Windows/Linux/Steam Deck/ROG Ally).
# Emscripten targets WASM32 by design, so skip this check under emcc.
if(NOT EMSCRIPTEN AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ding_snes_core targets x86_64 only (got a non-64-bit toolchain)")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── ding-core-sdk (submoduled as sdk/) ───────────────────────────────────────
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
if(NOT EXISTS ${SDK_DIR}/ding_core.h)
    message(FATAL_ERROR
        "sdk/ is missing ding-core-sdk headers. Did you forget:\n"
        "  git submodule update --init --recursive")
endif()

set(SDK_SOURCES
    ${SDK_DIR}/ding_md5.cpp
    ${SDK_DIR}/ding_savestate.cpp
    ${SDK_DIR}/ding_audio.cpp
)

# ── Core sources ──────────────────────────────────────────────────────────────
set(CORE_SOURCES
    src/Cartridge.cpp
    src/Bus.cpp
    src/CPU65816.cpp
    src/PPU.cpp
    src/SPC700.cpp
    src/SNES.cpp
    src/ding_core_snes.cpp
)

set(ALL_SOURCES ${CORE_SOURCES} ${SDK_SOURCES})

# ── Target ───────────────────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Single WASM+JS glue output. MODULARIZE lets the frontend `await
    # createDingSnesCore()` instead of polluting the global scope — matches
    # how genesis-core/gameboy-core's WASM builds are consumed already.
    add_executable(ding_snes_core ${ALL_SOURCES})
    target_link_options(ding_snes_core PRIVATE
        "SHELL:-s WASM=1"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=createDingSnesCore"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s ENVIRONMENT=web"
    )
else()
    add_library(ding_snes_core SHARED ${ALL_SOURCES})
    set_target_properties(ding_snes_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        PREFIX ""            # -> ding_snes_core.dll / ding_snes_core.so, no lib prefix
    )
endif()

target_include_directories(ding_snes_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SDK_DIR}
)

if(MSVC)
    target_compile_options(ding_snes_core PRIVATE /W4)
else()
    target_compile_options(ding_snes_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ── Optional: quick compile-only sanity target for CI ────────────────────────
# `cmake --build build --target ding_snes_core_check` — same sources, no link
# step, just confirms every .cpp still compiles against the current headers.
add_custom_target(ding_snes_core_check
    COMMAND ${CMAKE_COMMAND} -E echo "ding_snes_core: sources compiled as part of the main target"
    DEPENDS ding_snes_core
)
