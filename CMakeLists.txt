# NES Emulator - CMake Build System
# ==================================
#
# Out-of-source build (required):
#   mkdir build && cd build && cmake .. && make
#
# Directory structure:
#   src/              - Core implementation
#   tests/            - Test files
#   frontends/        - Platform-specific frontends
#
# This build system produces:
#   - libnes_static: Static library with the emulator core
#   - Tests: test_cpu, test_ppu, test_nes, etc.
#   - Frontends: mynes, run_rom (headless)
#
# Usage:
#   mkdir cmake-build && cd cmake-build
#   cmake ..
#   cmake --build .
#   ctest
#
# Options:
#   -DNES_BUILD_TESTS=ON/OFF      Build tests (default: ON)
#   -DNES_BUILD_FRONTENDS=ON/OFF  Build frontends (default: ON)
#   -DMYNES_PORTABLE_BINARY=ON    Fixed ISA baseline instead of -march=native
#                                 (default: OFF; use for binaries you ship)

cmake_minimum_required(VERSION 3.16)

include(cmake/AppleSDK.cmake)

project(MyNES
    VERSION 0.3.0
    DESCRIPTION "Cycle-accurate NES emulator with DSL-based CPU"
    LANGUAGES C
)

# Reject in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed.\n"
        "Use: mkdir build && cd build && cmake ..\n"
        "Remove CMakeCache.txt and CMakeFiles/ if they were created.")
endif()

# Default to Release build (O3 + NDEBUG) if the user didn't pick a type.
# Without this, the default is an unoptimized build, which is a huge perf hit
# for the NTSC composite pipeline in particular.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Enable native CPU feature use in Release builds. On x86 this unlocks
# AVX2 + FMA for the NTSC FIR inner loops (big speedup); on Apple Silicon
# it uses the full NEON feature set. Guarded to Release so debug builds
# stay portable.
#
# A binary built with -march=native dies with an illegal instruction on an
# older machine, so distributions turn on MYNES_PORTABLE_BINARY: a fixed
# baseline that still carries what the FIR loops need (AVX2/FMA via
# x86-64-v3 on x86-64, the first Apple Silicon core on arm64 Macs).
option(MYNES_PORTABLE_BINARY
    "Target a fixed ISA baseline instead of -march=native (for binaries that run on other machines)" OFF)
set(MYNES_ARCH_FLAGS "")
set(MYNES_ARCH_NOTE "compiler default")
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND
   (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
    CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_C_COMPILER_ID STREQUAL "AppleClang"))
    if(NOT MYNES_PORTABLE_BINARY)
        set(MYNES_ARCH_FLAGS -march=native)
        set(MYNES_ARCH_NOTE "this machine only")
    else()
        set(_mynes_arch "${CMAKE_SYSTEM_PROCESSOR}")
        if(CMAKE_OSX_ARCHITECTURES)
            set(_mynes_arch "${CMAKE_OSX_ARCHITECTURES}")
        endif()
        if(APPLE AND _mynes_arch MATCHES "^(arm64|aarch64)$")
            set(MYNES_ARCH_FLAGS -mcpu=apple-m1)
            set(MYNES_ARCH_NOTE "Apple Silicon baseline")
        elseif(_mynes_arch MATCHES "^(x86_64|AMD64|amd64)$")
            set(MYNES_ARCH_FLAGS -march=x86-64-v3)
            set(MYNES_ARCH_NOTE "x86-64-v3: AVX2/FMA, Haswell 2013 and later")
        else()
            # Universal or unusual targets: leave the compiler's own baseline.
            set(MYNES_ARCH_NOTE "compiler default for ${_mynes_arch}")
        endif()
        unset(_mynes_arch)
    endif()
    if(MYNES_ARCH_FLAGS)
        add_compile_options(${MYNES_ARCH_FLAGS})
    endif()
endif()

# Options
option(NES_BUILD_TESTS "Build test executables" ON)
option(NES_BUILD_FRONTENDS "Build frontend applications" ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# Code Generation (CPU from DSL)
# =============================================================================

find_program(CSI_EXECUTABLE csi
    HINTS /usr/local/bin /opt/homebrew/bin
)

if(NOT CSI_EXECUTABLE)
    message(WARNING "Chicken Scheme (csi) not found. Using pre-generated CPU code if available.")
endif()

set(CPU_DSL_SOURCE ${CMAKE_SOURCE_DIR}/src/cpu/nes6502.dsl)
set(DSL_COMPILER ${CMAKE_SOURCE_DIR}/tools/dsl2c.scm)
set(CPU_GENERATED_C ${CMAKE_BINARY_DIR}/generated/cpu_gen.c)
set(CPU_GENERATED_H ${CMAKE_BINARY_DIR}/generated/cpu_gen.h)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

if(CSI_EXECUTABLE)
    add_custom_command(
        OUTPUT ${CPU_GENERATED_C} ${CPU_GENERATED_H}
        COMMAND ${CSI_EXECUTABLE} -s ${DSL_COMPILER} ${CPU_DSL_SOURCE} ${CPU_GENERATED_C} ${CPU_GENERATED_H}
        DEPENDS ${CPU_DSL_SOURCE} ${DSL_COMPILER}
        COMMENT "Generating CPU microcode from DSL (header + implementation)..."
        VERBATIM
    )
else()
    # Fallback: use pre-generated files committed in generated/
    if(EXISTS ${CMAKE_SOURCE_DIR}/generated/cpu_gen.c AND EXISTS ${CMAKE_SOURCE_DIR}/generated/cpu_gen.h)
        add_custom_command(
            OUTPUT ${CPU_GENERATED_C} ${CPU_GENERATED_H}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/generated/cpu_gen.c ${CPU_GENERATED_C}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/generated/cpu_gen.h ${CPU_GENERATED_H}
            DEPENDS ${CMAKE_SOURCE_DIR}/generated/cpu_gen.c ${CMAKE_SOURCE_DIR}/generated/cpu_gen.h
            COMMENT "Using pre-generated CPU code (install Chicken Scheme for DSL compilation)..."
        )
    else()
        message(FATAL_ERROR "Cannot generate CPU code: Chicken Scheme (csi) not found and no pre-generated files exist.\nInstall with: brew install chicken")
    endif()
endif()

add_custom_target(generate_cpu DEPENDS ${CPU_GENERATED_C} ${CPU_GENERATED_H})

# =============================================================================
# Core Library
# =============================================================================

# Interface library for header-only usage
add_library(nes INTERFACE)

target_include_directories(nes INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
)

add_dependencies(nes generate_cpu)

# Static library (includes generated CPU code)
add_library(nes_static STATIC
    ${CPU_GENERATED_C}
    src/nes/hooks.c
    src/nes/debug.c
    src/nes/trace.c
    src/nes/cpu_trace.c
    src/nes/screen_dump.c
    src/nes/script.c
    src/nes/mapper.c
    src/nes/state.c
    src/nes/mappers/mapper_nrom.c
    src/nes/mappers/mapper_mmc1.c
    src/nes/mappers/mapper_uxrom.c
    src/nes/mappers/mapper_cnrom.c
    src/nes/mappers/mapper_mmc3.c
    src/nes/mappers/mapper_mmc5.c
    src/nes/mappers/mapper_fme7.c
    src/nes/mappers/mapper_axrom.c
    src/nes/mappers/mapper_mmc4.c
    src/nes/mappers/mapper_227.c
    src/nes/mappers/mapper_mmc2.c
    src/nes/mappers/mapper_colordreams.c
    src/nes/mappers/mapper_bnrom.c
    src/nes/mappers/mapper_gxrom.c
    src/nes/mappers/mapper_camerica.c
    src/nes/mappers/mapper_namco108.c
    # Vendored LMP88959/PAL-CRT (permissive custom license) — provides
    # the authentic 2C07 PAL encoder/decoder used by the composite
    # pipeline when region = PAL. See src/nes/palcrt/README.md.
    src/nes/palcrt/pal_core.c
    src/nes/palcrt/pal_nes.c
)

target_include_directories(nes_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
)

add_dependencies(nes_static generate_cpu)

# The core uses libm (sin, exp, tanhf). macOS folds libm into libSystem, so a
# missing link is invisible there but breaks every Linux test executable.
find_library(NES_LIBM m)
if(NES_LIBM)
    target_link_libraries(nes_static PUBLIC ${NES_LIBM})
endif()

# Aliases
add_library(NES::nes ALIAS nes)
add_library(NES::nes_static ALIAS nes_static)

# Opt-in benchmark: built explicitly with --target bench_core.
add_executable(bench_core EXCLUDE_FROM_ALL tools/bench_core.c)
target_link_libraries(bench_core PRIVATE nes_static)

# =============================================================================
# Tests
# =============================================================================

if(NES_BUILD_TESTS)
    enable_testing()

    # CPU tests
    add_executable(test_cpu tests/cpu/test_cpu.c)
    target_link_libraries(test_cpu PRIVATE nes_static)
    add_test(NAME cpu_tests COMMAND test_cpu)

    # CPU cycle tests
    add_executable(test_cpu_cycles tests/cpu/test_cpu_cycles.c)
    target_link_libraries(test_cpu_cycles PRIVATE nes_static)
    add_test(NAME cpu_cycle_tests COMMAND test_cpu_cycles)

    # PPU tests
    add_executable(test_ppu tests/ppu/test_ppu.c)
    target_link_libraries(test_ppu PRIVATE nes_static)
    add_test(NAME ppu_tests COMMAND test_ppu)

    # NES integration tests
    add_executable(test_nes tests/nes/test_nes.c)
    target_link_libraries(test_nes PRIVATE nes_static)
    add_test(NAME nes_tests COMMAND test_nes)

    add_executable(test_apu tests/nes/test_apu.c)
    target_link_libraries(test_apu PRIVATE nes_static)
    add_test(NAME apu_tests COMMAND test_apu)

    add_executable(test_pal_apu_roms tests/nes/test_pal_apu_roms.c)
    target_link_libraries(test_pal_apu_roms PRIVATE nes_static)
    add_test(NAME pal_apu_rom_tests COMMAND test_pal_apu_roms)
    set_tests_properties(pal_apu_rom_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 120)

    # ROM loading tests
    add_executable(test_rom tests/nes/test_rom.c)
    target_link_libraries(test_rom PRIVATE nes_static)
    add_test(NAME rom_tests COMMAND test_rom)

    # Mapper 227 (1200-in-1 multicart) tests
    add_executable(test_mapper227 tests/nes/test_mapper227.c)
    target_link_libraries(test_mapper227 PRIVATE nes_static)
    add_test(NAME mapper227_tests COMMAND test_mapper227)

    # MMC1 reset/register semantics
    add_executable(test_mapper1 tests/nes/test_mapper1.c)
    target_link_libraries(test_mapper1 PRIVATE nes_static)
    add_test(NAME mapper1_tests COMMAND test_mapper1)

    add_executable(test_mapper5 tests/nes/test_mapper5.c)
    target_link_libraries(test_mapper5 PRIVATE nes_static)
    add_test(NAME mapper5_tests COMMAND test_mapper5)

    # MMC2 latch semantics
    add_executable(test_mapper9 tests/nes/test_mapper9.c)
    target_link_libraries(test_mapper9 PRIVATE nes_static)
    add_test(NAME mapper9_tests COMMAND test_mapper9)

    # Color Dreams bus conflicts
    add_executable(test_mapper11 tests/nes/test_mapper11.c)
    target_link_libraries(test_mapper11 PRIVATE nes_static)
    add_test(NAME mapper11_tests COMMAND test_mapper11)

    # BNROM / NINA-001 selection by CHR type
    add_executable(test_mapper34 tests/nes/test_mapper34.c)
    target_link_libraries(test_mapper34 PRIVATE nes_static)
    add_test(NAME mapper34_tests COMMAND test_mapper34)

    # GxROM
    add_executable(test_mapper66 tests/nes/test_mapper66.c)
    target_link_libraries(test_mapper66 PRIVATE nes_static)
    add_test(NAME mapper66_tests COMMAND test_mapper66)

    # Camerica/Codemasters incl. Fire Hawk mirroring
    add_executable(test_mapper71 tests/nes/test_mapper71.c)
    target_link_libraries(test_mapper71 PRIVATE nes_static)
    add_test(NAME mapper71_tests COMMAND test_mapper71)

    # Namco 108 fixed layout, no IRQ/mirroring/PRG RAM
    add_executable(test_mapper206 tests/nes/test_mapper206.c)
    target_link_libraries(test_mapper206 PRIVATE nes_static)
    add_test(NAME mapper206_tests COMMAND test_mapper206)

    # Save-state round trips against the bundled AccuracyCoin ROM
    add_executable(test_state tests/nes/test_state.c)
    target_link_libraries(test_state PRIVATE nes_static)
    add_test(NAME state_tests COMMAND test_state)
    set_tests_properties(state_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 120)

    # AccuracyCoin test runner
    add_executable(accuracy_coin tests/accuracy_coin/run_accuracy.c)
    target_link_libraries(accuracy_coin PRIVATE nes_static)
    add_test(NAME accuracy_coin_tests COMMAND accuracy_coin)
    set_tests_properties(accuracy_coin_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 120)

    # Convenience targets
    add_custom_target(cputest COMMAND test_cpu DEPENDS test_cpu)
    add_custom_target(pputest COMMAND test_ppu DEPENDS test_ppu)
    add_custom_target(nestest COMMAND test_nes DEPENDS test_nes)
    add_custom_target(alltests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS test_cpu test_cpu_cycles test_ppu test_nes test_rom accuracy_coin)
endif()

# =============================================================================
# Frontends
# =============================================================================

if(NES_BUILD_FRONTENDS)
    # Shared frontend helpers (config persistence + ROM browser).
    # Header-only-ish: these compile once into each frontend that needs them.
    set(MYNES_SHARED_SOURCES
        frontends/shared/config.c
        frontends/shared/browser.c
        frontends/shared/saves.c
    )

    if(NES_BUILD_TESTS)
        add_executable(test_browser frontends/shared/tests/test_browser.c frontends/shared/browser.c)
        target_include_directories(test_browser PRIVATE frontends/shared)
        target_link_libraries(test_browser PRIVATE nes_static)
        add_test(NAME browser_tests COMMAND test_browser)

        # Battery-file naming, atomic writes and state slot paths
        add_executable(test_saves frontends/shared/tests/test_saves.c
            frontends/shared/saves.c frontends/shared/config.c)
        target_include_directories(test_saves PRIVATE frontends/shared)
        target_link_libraries(test_saves PRIVATE nes_static)
        add_test(NAME saves_tests COMMAND test_saves)

        # config.json defaults and the stored render scale
        add_executable(test_config frontends/shared/tests/test_config.c frontends/shared/config.c)
        target_include_directories(test_config PRIVATE frontends/shared)
        target_link_libraries(test_config PRIVATE nes_static)
        add_test(NAME config_tests COMMAND test_config)
    endif()

    # SDL2 Frontend
    find_package(SDL2 QUIET)

    if(SDL2_FOUND)
        add_executable(mynes frontends/sdl/main.c ${MYNES_SHARED_SOURCES})
        target_include_directories(mynes PRIVATE frontends/shared)
        target_link_libraries(mynes PRIVATE nes_static SDL2::SDL2)
        message(STATUS "SDL2 found - building SDL frontend")
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
            if(SDL2_FOUND)
                add_executable(mynes frontends/sdl/main.c ${MYNES_SHARED_SOURCES})
                target_include_directories(mynes PRIVATE
                    ${SDL2_INCLUDE_DIRS} frontends/shared)
                target_link_libraries(mynes PRIVATE nes_static PkgConfig::SDL2)
                message(STATUS "SDL2 found via pkg-config - building SDL frontend")
            else()
                message(STATUS "SDL2 not found - skipping SDL frontend")
            endif()
        else()
            message(STATUS "SDL2 not found - skipping SDL frontend")
        endif()
    endif()

endif()

# =============================================================================
# Headless Frontend & Test Runner
# =============================================================================

if(NES_BUILD_FRONTENDS)
    # Headless ROM runner
    add_executable(run_rom frontends/headless/main.c)
    target_link_libraries(run_rom PRIVATE nes_static)

    # Test runner (Blargg detection, scripting, traces)
    add_executable(test_runner tools/test_runner.c)
    target_link_libraries(test_runner PRIVATE nes_static)
endif()

# =============================================================================
# GPU Frontend (SDL3 + SDL_GPU — Vulkan/Metal/D3D12)
# =============================================================================

option(NES_BUILD_GPU_FRONTEND "Build the GPU frontend (requires SDL3)" ON)
option(MYNES_BUNDLED_SDL3 "Build pinned SDL3 with Metal fence fixes" ${APPLE})

if(NES_BUILD_FRONTENDS AND NES_BUILD_GPU_FRONTEND)
    if(MYNES_BUNDLED_SDL3)
        include(cmake/BundledSDL3.cmake)
    else()
        find_package(SDL3 CONFIG QUIET)
    endif()
    if(TARGET SDL3::SDL3)
        set(MYNES_SDL3_TARGET SDL3::SDL3)
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
        endif()
        if(TARGET PkgConfig::SDL3)
            set(MYNES_SDL3_TARGET PkgConfig::SDL3)
        else()
            find_path(MYNES_SDL3_INCLUDE_DIR SDL3/SDL.h
                HINTS /opt/homebrew/include /usr/local/include)
            find_library(MYNES_SDL3_LIBRARY SDL3
                HINTS /opt/homebrew/lib /usr/local/lib)
            if(MYNES_SDL3_INCLUDE_DIR AND MYNES_SDL3_LIBRARY)
                add_library(mynes_sdl3 UNKNOWN IMPORTED)
                set_target_properties(mynes_sdl3 PROPERTIES
                    IMPORTED_LOCATION "${MYNES_SDL3_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${MYNES_SDL3_INCLUDE_DIR}")
                set(MYNES_SDL3_TARGET mynes_sdl3)
            endif()
        endif()
    endif()

    if(MYNES_SDL3_TARGET)
        add_executable(mynes_gpu
            frontends/gpu/main.c
            frontends/gpu/gpu_osd.c
            frontends/gpu/gpu_benchmark.c
            frontends/gpu/playback.c
            frontends/gpu/gpu_render.c
            frontends/gpu/frame_capture.c
            frontends/gpu/recorder.c
            frontends/gpu/frame_pq.c
            frontends/gpu/gpu_output.c
            frontends/gpu/preset_apply.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/gpu_display.c
            frontends/gpu/video_gpu.c
            frontends/gpu/vhs_deck.c
            frontends/gpu/vhs_gpu.c
            frontends/gpu/chroma_pipeline.c
            frontends/gpu/post_pipeline.c
            frontends/gpu/signal_chain.c
            frontends/gpu/audio_chain.c
            frontends/gpu/audio_gpu.c
            frontends/gpu/video_chain.c
            frontends/gpu/chain_vis.c
            frontends/gpu/debug_tap.c
            frontends/gpu/debug_server.c
            frontends/gpu/gpu_log.c
            ${MYNES_SHARED_SOURCES}
        )
        target_include_directories(mynes_gpu PRIVATE
            frontends/shared)
        target_link_libraries(mynes_gpu PRIVATE nes_static ${MYNES_SDL3_TARGET} pthread)
        if(MYNES_BUNDLED_SDL3)
            target_compile_definitions(mynes_gpu PRIVATE MYNES_BUNDLED_SDL3=1)
        endif()
        if(APPLE)
            enable_language(OBJC)
            target_sources(mynes_gpu PRIVATE frontends/gpu/gpu_output_macos.m)
            target_link_libraries(mynes_gpu PRIVATE "-framework AppKit" "-framework CoreGraphics")
        endif()
        message(STATUS "SDL3 found - building GPU frontend")

        # A picture or a video through a preset's whole chain, from the RGB
        # encoder source with linear 10-bit guns, recorded like
        # mynes_gpu --record. See frontends/video/main.c.
        add_executable(mynes_video
            frontends/video/main.c
            frontends/gpu/gpu_osd.c
            frontends/gpu/gpu_render.c
            frontends/gpu/frame_capture.c
            frontends/gpu/recorder.c
            frontends/gpu/frame_pq.c
            frontends/gpu/gpu_output.c
            frontends/gpu/preset_apply.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/gpu_display.c
            frontends/gpu/video_gpu.c
            frontends/gpu/vhs_deck.c
            frontends/gpu/vhs_gpu.c
            frontends/gpu/chroma_pipeline.c
            frontends/gpu/post_pipeline.c
            frontends/gpu/signal_chain.c
            frontends/gpu/audio_chain.c
            frontends/gpu/audio_gpu.c
            frontends/gpu/video_chain.c
            frontends/gpu/chain_vis.c
            frontends/gpu/debug_tap.c
            frontends/gpu/debug_server.c
            frontends/gpu/gpu_log.c
            ${MYNES_SHARED_SOURCES}
        )
        target_include_directories(mynes_video PRIVATE frontends/shared frontends/gpu)
        target_link_libraries(mynes_video PRIVATE nes_static ${MYNES_SDL3_TARGET} pthread)
        if(MYNES_BUNDLED_SDL3)
            target_compile_definitions(mynes_video PRIVATE MYNES_BUNDLED_SDL3=1)
        endif()
        if(APPLE)
            target_sources(mynes_video PRIVATE frontends/gpu/gpu_output_macos.m)
            target_link_libraries(mynes_video PRIVATE "-framework AppKit" "-framework CoreGraphics")
        endif()

        # GPU shaders: GLSL -> SPIR-V -> MSL into build/shaders/{compute,render}.
        # compile_shaders.sh needs glslc (shaderc), spirv-cross and python3, and
        # also repairs the Metal buffer indices in the MSL it writes. Every
        # compiled shader is committed next to its .glsl, so a machine without
        # the tools copies those instead of failing. Neither path writes into
        # the source tree; the shaders_regenerate target below is the only
        # thing that does, for maintainers who changed a .glsl.
        set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/frontends/gpu/shaders)
        set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
        file(MAKE_DIRECTORY ${SHADER_OUT_DIR}/compute)
        file(MAKE_DIRECTORY ${SHADER_OUT_DIR}/render)

        # An IDE can hand CMake a PATH without Homebrew's bin directory, and
        # CMake's own search skips /opt/homebrew in some setups (3.28
        # configuring for x86_64 on Apple silicon does), which sent a Mac with
        # both tools installed to the copy path below. PATHS is searched after
        # PATH, so a glslc on PATH still takes precedence.
        find_program(GLSLC_EXECUTABLE glslc PATHS /opt/homebrew/bin /usr/local/bin)
        find_program(SPIRV_CROSS_EXECUTABLE spirv-cross PATHS /opt/homebrew/bin /usr/local/bin)
        find_program(MYNES_PYTHON3 NAMES python3)

        # Stamp file tracks when build/shaders was last populated.
        # mynes_shader_step from cmake/ShaderStep.cmake adds the step that
        # writes it, on either path below.
        include(${CMAKE_SOURCE_DIR}/cmake/ShaderStep.cmake)
        set(SHADER_STAMP ${SHADER_OUT_DIR}/.compile_stamp)
        if(GLSLC_EXECUTABLE AND SPIRV_CROSS_EXECUTABLE AND MYNES_PYTHON3)
            set(MYNES_SHADER_SOURCE "compiled with ${GLSLC_EXECUTABLE}")
            message(STATUS "GPU shaders: compiling GLSL with ${GLSLC_EXECUTABLE}, "
                "${SPIRV_CROSS_EXECUTABLE} and ${MYNES_PYTHON3}")
            # Hand the script the exact tools CMake found rather than whatever
            # PATH resolves to at build time.
            set(SHADER_TOOL_ENV ${CMAKE_COMMAND} -E env
                GLSLC=${GLSLC_EXECUTABLE}
                SPIRV_CROSS=${SPIRV_CROSS_EXECUTABLE}
                PYTHON3=${MYNES_PYTHON3})
            mynes_shader_step(
                SRC_DIR ${SHADER_SRC_DIR} OUT_DIR ${SHADER_OUT_DIR} STAMP ${SHADER_STAMP}
                FROM GLSL
                COMMENT "Compiling GPU shaders (GLSL -> SPIR-V -> MSL)"
                COMMAND ${SHADER_TOOL_ENV} ${SHADER_SRC_DIR}/compile_shaders.sh ${SHADER_OUT_DIR}
                DEPENDS ${SHADER_SRC_DIR}/compile_shaders.sh
            )
            add_custom_target(shaders_regenerate
                COMMAND ${SHADER_TOOL_ENV} ${SHADER_SRC_DIR}/compile_shaders.sh ${SHADER_SRC_DIR}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Regenerating the committed SPIR-V/MSL shaders in the source tree"
                VERBATIM
            )
        else()
            set(MYNES_SHADER_MISSING "")
            if(NOT GLSLC_EXECUTABLE)
                list(APPEND MYNES_SHADER_MISSING glslc)
            endif()
            if(NOT SPIRV_CROSS_EXECUTABLE)
                list(APPEND MYNES_SHADER_MISSING spirv-cross)
            endif()
            if(NOT MYNES_PYTHON3)
                list(APPEND MYNES_SHADER_MISSING python3)
            endif()
            list(JOIN MYNES_SHADER_MISSING ", " MYNES_SHADER_MISSING)
            set(MYNES_SHADER_SOURCE "committed SPIR-V/MSL (not found: ${MYNES_SHADER_MISSING})")
            message(STATUS "GPU shaders: copying the committed SPIR-V/MSL, not found: "
                "${MYNES_SHADER_MISSING}; to use a tool installed elsewhere, set "
                "GLSLC_EXECUTABLE, SPIRV_CROSS_EXECUTABLE or MYNES_PYTHON3")
            mynes_shader_step(
                SRC_DIR ${SHADER_SRC_DIR} OUT_DIR ${SHADER_OUT_DIR} STAMP ${SHADER_STAMP}
                FROM COMMITTED
                COMMENT "Copying committed GPU shaders (shader tools not installed)"
                COMMAND ${CMAKE_COMMAND}
                    -DSHADER_SRC_DIR=${SHADER_SRC_DIR}
                    -DSHADER_OUT_DIR=${SHADER_OUT_DIR}
                    -P ${CMAKE_SOURCE_DIR}/cmake/CopyPrebuiltShaders.cmake
                DEPENDS ${CMAKE_SOURCE_DIR}/cmake/CopyPrebuiltShaders.cmake
            )
            add_custom_target(shaders_regenerate
                COMMAND ${CMAKE_COMMAND} -E echo
                    "shaders_regenerate needs glslc (shaderc), spirv-cross and python3; install them and re-run cmake"
                COMMAND ${CMAKE_COMMAND} -E false
                VERBATIM
            )
        endif()
        add_custom_target(gpu_shaders DEPENDS ${SHADER_STAMP})
        add_dependencies(mynes_gpu gpu_shaders)
        add_dependencies(mynes_video gpu_shaders)
        # Keep presets and palettes with the executable, independent of the
        # launch directory (release trees and the app bundle use the same layout).
        add_custom_target(gpu_presets
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/presets ${CMAKE_BINARY_DIR}/presets
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/palettes ${CMAKE_BINARY_DIR}/palettes
            COMMENT "Copying GPU preset library and palettes")
        add_dependencies(mynes_gpu gpu_presets)
        add_dependencies(mynes_video gpu_presets)

        # Research prototype: a libretro core (Mega Drive, Super Famicom)
        # through the same signal, receiver and CRT chain, with the RGB
        # encoder source in place of the 2C02 DAC. See frontends/retro/main.c.
        add_executable(mynes_retro
            frontends/retro/main.c
            frontends/gpu/gpu_osd.c
            frontends/gpu/gpu_render.c
            frontends/gpu/frame_capture.c
            frontends/gpu/recorder.c
            frontends/gpu/frame_pq.c
            frontends/gpu/gpu_output.c
            frontends/gpu/preset_apply.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/gpu_display.c
            frontends/gpu/video_gpu.c
            frontends/gpu/vhs_deck.c
            frontends/gpu/vhs_gpu.c
            frontends/gpu/chroma_pipeline.c
            frontends/gpu/post_pipeline.c
            frontends/gpu/signal_chain.c
            frontends/gpu/audio_chain.c
            frontends/gpu/audio_gpu.c
            frontends/gpu/video_chain.c
            frontends/gpu/chain_vis.c
            frontends/gpu/debug_tap.c
            frontends/gpu/debug_server.c
            frontends/gpu/gpu_log.c
            ${MYNES_SHARED_SOURCES}
        )
        target_include_directories(mynes_retro PRIVATE frontends/shared frontends/gpu)
        target_link_libraries(mynes_retro PRIVATE nes_static ${MYNES_SDL3_TARGET} pthread)
        if(MYNES_BUNDLED_SDL3)
            target_compile_definitions(mynes_retro PRIVATE MYNES_BUNDLED_SDL3=1)
        endif()
        if(APPLE)
            target_sources(mynes_retro PRIVATE frontends/gpu/gpu_output_macos.m)
            target_link_libraries(mynes_retro PRIVATE "-framework AppKit" "-framework CoreGraphics")
        endif()
        add_dependencies(mynes_retro gpu_shaders gpu_presets)

        if(NES_BUILD_TESTS)
            add_executable(test_frame_capture frontends/gpu/tests/test_frame_capture.c
                frontends/gpu/frame_capture.c)
            target_include_directories(test_frame_capture PRIVATE frontends/gpu)
            target_link_libraries(test_frame_capture PRIVATE ${MYNES_SDL3_TARGET} m)
            add_test(NAME gpu_frame_capture_tests COMMAND test_frame_capture)

            # GPU kernel tests (no SDL3 dependency)
            add_executable(test_gpu_kernels frontends/gpu/tests/test_kernels.c)
            target_include_directories(test_gpu_kernels PRIVATE frontends/gpu)
            target_link_libraries(test_gpu_kernels PRIVATE m)
            add_test(NAME gpu_kernel_tests COMMAND test_gpu_kernels)

            add_executable(test_presentation frontends/gpu/tests/test_presentation.c)
            target_include_directories(test_presentation PRIVATE frontends/gpu)
            target_link_libraries(test_presentation PRIVATE m)
            add_test(NAME gpu_presentation_tests COMMAND test_presentation)

            # Picture placement: safe area in pixels, tube-face fit
            add_executable(test_output frontends/gpu/tests/test_output.c)
            target_include_directories(test_output PRIVATE frontends/gpu)
            target_link_libraries(test_output PRIVATE ${MYNES_SDL3_TARGET} m)
            add_test(NAME gpu_output_tests COMMAND test_output)

            # Held gamepad directions in the menu and ROM browser
            add_executable(test_nav_repeat frontends/gpu/tests/test_nav_repeat.c)
            target_include_directories(test_nav_repeat PRIVATE frontends/gpu)
            add_test(NAME gpu_nav_repeat_tests COMMAND test_nav_repeat)

            # Signal precompute tests (DAC model, FIR design, windows)
            add_executable(test_signal_precompute frontends/gpu/tests/test_signal_precompute.c)
            target_include_directories(test_signal_precompute PRIVATE frontends/gpu)
            target_link_libraries(test_signal_precompute PRIVATE m)
            add_test(NAME gpu_signal_precompute_tests COMMAND test_signal_precompute)

            # Video-chain helper tests (comb enum mapping, half-float conversion)
            add_executable(test_video_chain_helpers
                frontends/gpu/tests/test_video_chain_helpers.c
                frontends/gpu/video_chain.c)
            target_include_directories(test_video_chain_helpers PRIVATE frontends/gpu)
            target_link_libraries(test_video_chain_helpers PRIVATE m)
            add_test(NAME gpu_video_chain_helper_tests COMMAND test_video_chain_helpers)

            # JSON preset validation (ranges, invariants) — scans presets/*.json
            add_executable(test_presets
                frontends/gpu/tests/test_presets.c
                frontends/gpu/video_chain.c)
            target_include_directories(test_presets PRIVATE frontends/gpu)
            target_link_libraries(test_presets PRIVATE m)
            add_test(NAME gpu_preset_tests
                COMMAND test_presets
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

            if(MYNES_PYTHON3)
                add_test(NAME gpu_fw900_measurements
                    COMMAND ${MYNES_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/measurements/build_fw900_model.py --check)
                add_test(NAME gpu_preset_control_audit
                    COMMAND ${MYNES_PYTHON3} ${CMAKE_SOURCE_DIR}/frontends/gpu/tests/test_preset_controls.py)
            endif()

            # VHS deck host model: filter design, transport table, dropouts
            add_executable(test_vhs_deck
                frontends/gpu/tests/test_vhs_deck.c
                frontends/gpu/vhs_deck.c)
            target_include_directories(test_vhs_deck PRIVATE frontends/gpu)
            target_link_libraries(test_vhs_deck PRIVATE ${MYNES_SDL3_TARGET} m)
            add_test(NAME gpu_vhs_deck_tests COMMAND test_vhs_deck)

            # JSON preset save/load round-trip + string enum tests
            add_executable(test_preset_json
                frontends/gpu/tests/test_preset_json.c
                frontends/gpu/video_chain.c)
            target_include_directories(test_preset_json PRIVATE frontends/gpu)
            target_link_libraries(test_preset_json PRIVATE m)
            add_test(NAME gpu_preset_json_tests
                COMMAND test_preset_json
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

            add_executable(test_audio
                frontends/gpu/tests/test_audio.c
                frontends/gpu/audio_chain.c frontends/gpu/audio_gpu.c
                frontends/gpu/gpu_compute.c frontends/gpu/gpu_log.c)
            target_include_directories(test_audio PRIVATE frontends/gpu)
            target_link_libraries(test_audio PRIVATE ${MYNES_SDL3_TARGET} m)
            add_dependencies(test_audio gpu_shaders)
            add_test(NAME gpu_audio_tests COMMAND test_audio)

            add_executable(test_gpu_fences frontends/gpu/tests/test_gpu_fences.c)
            target_link_libraries(test_gpu_fences PRIVATE ${MYNES_SDL3_TARGET})
            add_test(NAME gpu_fence_tests COMMAND test_gpu_fences)
            set_tests_properties(gpu_fence_tests PROPERTIES TIMEOUT 45)

            add_executable(test_playback frontends/gpu/tests/test_playback.c
                frontends/gpu/playback.c frontends/gpu/audio_chain.c
                frontends/gpu/audio_gpu.c frontends/gpu/gpu_compute.c frontends/gpu/gpu_log.c)
            target_include_directories(test_playback PRIVATE frontends/gpu)
            target_link_libraries(test_playback PRIVATE nes_static ${MYNES_SDL3_TARGET} m)
            add_test(NAME gpu_playback_tests COMMAND test_playback)
            # 77: every check passed, but the host starved the playback worker
            # and fast-forward speed could not be checked.
            set_tests_properties(gpu_playback_tests PROPERTIES SKIP_RETURN_CODE 77)

            # Clip recorder: frame arithmetic, ffmpeg command lines, the
            # rawvideo/audio muxing helper (runs ffmpeg and ffprobe when they
            # are on PATH), the worker's capture window and input recording.
            add_executable(test_recorder frontends/gpu/tests/test_recorder.c
                frontends/gpu/recorder.c frontends/gpu/frame_pq.c frontends/gpu/frame_capture.c
                frontends/gpu/playback.c frontends/gpu/audio_chain.c
                frontends/gpu/audio_gpu.c frontends/gpu/gpu_compute.c frontends/gpu/gpu_log.c)
            target_include_directories(test_recorder PRIVATE frontends/gpu)
            target_link_libraries(test_recorder PRIVATE nes_static ${MYNES_SDL3_TARGET} m)
            add_test(NAME gpu_recorder_tests COMMAND test_recorder)
            set_tests_properties(gpu_recorder_tests PROPERTIES TIMEOUT 120)

            # HDR recording conversion: PQ curve, BT.2020 matrix, lookup
            # accuracy, every half-float input and the frame light statistics.
            add_executable(test_frame_pq frontends/gpu/tests/test_frame_pq.c
                frontends/gpu/frame_pq.c)
            target_include_directories(test_frame_pq PRIVATE frontends/gpu)
            target_link_libraries(test_frame_pq PRIVATE m pthread)
            add_test(NAME gpu_frame_pq_tests COMMAND test_frame_pq)

            # GPU pipeline integration test (headless SDL3 GPU compute)
            add_executable(test_pipeline
                frontends/gpu/tests/test_pipeline.c
                frontends/gpu/gpu_compute.c
                frontends/gpu/video_gpu.c
                frontends/gpu/vhs_deck.c
                frontends/gpu/vhs_gpu.c
                frontends/gpu/chroma_pipeline.c
                frontends/gpu/post_pipeline.c
                frontends/gpu/signal_chain.c
                frontends/gpu/video_chain.c
                frontends/gpu/gpu_log.c
            )
            target_include_directories(test_pipeline PRIVATE
                frontends/gpu
            )
            target_link_libraries(test_pipeline PRIVATE nes_static ${MYNES_SDL3_TARGET} m)
            add_dependencies(test_pipeline gpu_shaders)
            add_test(NAME gpu_pipeline_test COMMAND test_pipeline)
            get_target_property(PIPELINE_TEST_SOURCES test_pipeline SOURCES)
            list(REMOVE_ITEM PIPELINE_TEST_SOURCES frontends/gpu/tests/test_pipeline.c)
            add_executable(test_fidelity frontends/gpu/tests/test_fidelity.c frontends/gpu/tests/test_encoder.c frontends/gpu/tests/test_display_fidelity.c frontends/gpu/tests/test_vhs_fidelity.c frontends/gpu/tests/test_crt_load.c frontends/gpu/tests/test_osd.c frontends/gpu/tests/test_border.c frontends/gpu/gpu_osd.c frontends/shared/browser.c frontends/gpu/gpu_display.c ${PIPELINE_TEST_SOURCES})
            target_include_directories(test_fidelity PRIVATE frontends/gpu frontends/shared)
            target_compile_definitions(test_fidelity PRIVATE MYNES_TEST_PRESET_DIR="${CMAKE_SOURCE_DIR}/presets")
            target_link_libraries(test_fidelity PRIVATE nes_static ${MYNES_SDL3_TARGET} m)
            add_dependencies(test_fidelity gpu_shaders)
            add_test(NAME gpu_fidelity_tests COMMAND test_fidelity)
            if(MYNES_PYTHON3)
                # mynes_video end to end; skipped (77) without ffmpeg.
                add_test(NAME gpu_video_tool_test
                    COMMAND ${MYNES_PYTHON3} ${CMAKE_SOURCE_DIR}/frontends/gpu/tests/test_video_tool.py
                            $<TARGET_FILE:mynes_video>)
                set_tests_properties(gpu_video_tool_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
            endif()

            # build/shaders must hold exactly the committed shader set, whichever
            # way it was populated, because the frontend loads by file name.
            add_test(NAME gpu_shader_layout_test
                COMMAND ${CMAKE_COMMAND}
                    -DSHADER_SRC_DIR=${SHADER_SRC_DIR}
                    -DSHADER_OUT_DIR=${SHADER_OUT_DIR}
                    -P ${CMAKE_SOURCE_DIR}/cmake/CheckShaderLayout.cmake)
            add_test(NAME gpu_shader_prune_test
                COMMAND ${CMAKE_COMMAND}
                    -DWORK_DIR=${CMAKE_BINARY_DIR}/shader_prune_test
                    -DPRUNE_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/PruneShaderOutputs.cmake
                    -P ${CMAKE_SOURCE_DIR}/cmake/TestPruneShaderOutputs.cmake)
            # A shader renamed with its mtime kept must still be rebuilt under
            # its new name, with the old outputs gone.
            add_test(NAME gpu_shader_rename_test
                COMMAND ${CMAKE_COMMAND}
                    -DWORK_DIR=${CMAKE_BINARY_DIR}/shader_rename_test
                    -DMODULE_DIR=${CMAKE_SOURCE_DIR}/cmake
                    -DGENERATOR=${CMAKE_GENERATOR}
                    -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                    -P ${CMAKE_SOURCE_DIR}/cmake/TestShaderRename.cmake)
        endif()

        # GPU image processor — run real pipeline on PNG screenshots
        add_executable(gpu_process
            tools/gpu_process.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/video_gpu.c
            frontends/gpu/vhs_deck.c
            frontends/gpu/vhs_gpu.c
            frontends/gpu/chroma_pipeline.c
            frontends/gpu/post_pipeline.c
            frontends/gpu/signal_chain.c
            frontends/gpu/video_chain.c
            frontends/gpu/gpu_log.c
        )
        target_include_directories(gpu_process PRIVATE
            frontends/gpu
        )
        target_link_libraries(gpu_process PRIVATE nes_static ${MYNES_SDL3_TARGET} m)
        add_dependencies(gpu_process gpu_shaders)
    else()
        message(WARNING "SDL3 not found - GPU frontend unavailable. On macOS: brew install sdl3 shaderc spirv-cross. Use -DNES_BUILD_GPU_FRONTEND=OFF to explicitly disable it.")
    endif()
endif()

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "MyNES Configuration:")
message(STATUS "  Version:         ${PROJECT_VERSION}")
message(STATUS "  Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build tests:     ${NES_BUILD_TESTS}")
message(STATUS "  Build frontends: ${NES_BUILD_FRONTENDS}")
message(STATUS "  CSI found:       ${CSI_EXECUTABLE}")
if(MYNES_ARCH_FLAGS)
    message(STATUS "  Portable binary: ${MYNES_PORTABLE_BINARY} (${MYNES_ARCH_FLAGS}: ${MYNES_ARCH_NOTE})")
else()
    message(STATUS "  Portable binary: ${MYNES_PORTABLE_BINARY} (${MYNES_ARCH_NOTE})")
endif()
if(MYNES_SHADER_SOURCE)
    message(STATUS "  GPU shaders:     ${MYNES_SHADER_SOURCE}")
endif()
message(STATUS "")

# Optional Tang Nano 20K USB output for the SDL frontend.
option(NES_CRT_USB "Build SDL frontend with Tang USB streaming/capture" OFF)
if(NES_CRT_USB)
    if(NOT TARGET mynes)
        message(FATAL_ERROR "NES_CRT_USB requires the SDL2 frontend")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CRT_USB REQUIRED IMPORTED_TARGET libusb-1.0)
    find_package(Threads REQUIRED)
    target_sources(mynes PRIVATE frontends/sdl/crt/crt_protocol.c
        frontends/sdl/crt/crt_live.c)
    target_link_libraries(mynes PRIVATE PkgConfig::CRT_USB Threads::Threads)
    target_include_directories(mynes PRIVATE frontends/sdl/crt)
    target_compile_definitions(mynes PRIVATE MYNES_CRT_CAPTURE=1)
endif()

if(NES_CRT_USB AND NES_BUILD_TESTS)
    add_executable(test_crt_protocol tests/test_crt_protocol.c frontends/sdl/crt/crt_protocol.c)
    target_include_directories(test_crt_protocol PRIVATE frontends/sdl/crt)
    add_test(NAME crt_protocol COMMAND test_crt_protocol)
endif()
