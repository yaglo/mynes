# NES Emulator - CMake Build System
# ==================================
#
# Out-of-source build (required):
#   mkdir build && cd build && cmake .. && make
#
# Directory structure:
#   include/nes/      - Public API headers
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

cmake_minimum_required(VERSION 3.16)

project(MyNES
    VERSION 0.2.0
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
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND
   (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
    CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_C_COMPILER_ID STREQUAL "AppleClang"))
    add_compile_options(-march=native)
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

# Note: src comes before include for backward compatibility
# (implementation headers have same names as public API headers)
target_include_directories(nes INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>
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
    src/nes/mappers/mapper_nrom.c
    src/nes/mappers/mapper_mmc1.c
    src/nes/mappers/mapper_uxrom.c
    src/nes/mappers/mapper_cnrom.c
    src/nes/mappers/mapper_mmc3.c
    src/nes/mappers/mapper_mmc5.c
    src/nes/mappers/mapper_fme7.c
    src/nes/mappers/mapper_axrom.c
    src/nes/mappers/mapper_mmc4.c
    # Vendored LMP88959/PAL-CRT (permissive custom license) — provides
    # the authentic 2C07 PAL encoder/decoder used by the composite
    # pipeline when region = PAL. See src/nes/palcrt/README.md.
    src/nes/palcrt/pal_core.c
    src/nes/palcrt/pal_nes.c
)

target_include_directories(nes_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>
)

add_dependencies(nes_static generate_cpu)

# Aliases
add_library(NES::nes ALIAS nes)
add_library(NES::nes_static ALIAS nes_static)

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

    # ROM loading tests
    add_executable(test_rom tests/nes/test_rom.c)
    target_link_libraries(test_rom PRIVATE nes_static)
    add_test(NAME rom_tests COMMAND test_rom)

    # AccuracyCoin test runner
    add_executable(accuracy_coin tests/accuracy_coin/run_accuracy.c)
    target_link_libraries(accuracy_coin PRIVATE nes_static)
    add_test(NAME accuracy_coin_tests COMMAND accuracy_coin)
    set_tests_properties(accuracy_coin_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
    )

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
            pkg_check_modules(SDL2 sdl2)
            if(SDL2_FOUND)
                add_executable(mynes frontends/sdl/main.c ${MYNES_SHARED_SOURCES})
                target_include_directories(mynes PRIVATE
                    ${SDL2_INCLUDE_DIRS} frontends/shared)
                target_link_libraries(mynes PRIVATE nes_static ${SDL2_LIBRARIES})
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

option(NES_BUILD_GPU_FRONTEND "Build the GPU frontend (requires SDL3)" OFF)

if(NES_BUILD_GPU_FRONTEND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL3 sdl3)
    endif()

    if(NOT SDL3_FOUND)
        # Try direct find
        find_path(SDL3_INCLUDE_DIRS SDL3/SDL.h
            HINTS /opt/homebrew/include /usr/local/include)
        find_library(SDL3_LIBRARIES SDL3
            HINTS /opt/homebrew/lib /usr/local/lib)
        if(SDL3_INCLUDE_DIRS AND SDL3_LIBRARIES)
            set(SDL3_FOUND TRUE)
        endif()
    endif()

    if(SDL3_FOUND)
        add_executable(mynes_gpu
            frontends/gpu/main.c
            frontends/gpu/gpu_render.c
            frontends/gpu/preset_apply.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/gpu_display.c
            frontends/gpu/video_gpu.c
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
            ${SDL3_INCLUDE_DIRS} frontends/shared)
        target_link_libraries(mynes_gpu PRIVATE nes_static ${SDL3_LIBRARIES} pthread)
        message(STATUS "SDL3 found - building GPU frontend")

        # Compile GLSL shaders to SPIR-V + MSL at build time.
        # Uses compile_shaders.sh which handles: glslc, spirv-cross, MSL buffer
        # index fixing, and copying outputs to build/shaders/.
        set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/frontends/gpu/shaders)
        set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
        file(MAKE_DIRECTORY ${SHADER_OUT_DIR}/compute)
        file(MAKE_DIRECTORY ${SHADER_OUT_DIR}/render)

        file(GLOB GLSL_COMPUTE_SOURCES ${SHADER_SRC_DIR}/compute/*.comp.glsl)
        file(GLOB GLSL_VERT_SOURCES ${SHADER_SRC_DIR}/render/*.vert.glsl)
        file(GLOB GLSL_FRAG_SOURCES ${SHADER_SRC_DIR}/render/*.frag.glsl)
        set(ALL_GLSL_SOURCES ${GLSL_COMPUTE_SOURCES} ${GLSL_VERT_SOURCES} ${GLSL_FRAG_SOURCES})

        # Stamp file tracks when shaders were last compiled.
        set(SHADER_STAMP ${CMAKE_BINARY_DIR}/shaders/.compile_stamp)
        add_custom_command(
            OUTPUT ${SHADER_STAMP}
            COMMAND ${SHADER_SRC_DIR}/compile_shaders.sh
            COMMAND ${CMAKE_COMMAND} -E touch ${SHADER_STAMP}
            DEPENDS ${ALL_GLSL_SOURCES} ${SHADER_SRC_DIR}/compile_shaders.sh
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Compiling GPU shaders (GLSL -> SPIR-V -> MSL)"
        )
        add_custom_target(gpu_shaders DEPENDS ${SHADER_STAMP})
        add_dependencies(mynes_gpu gpu_shaders)

        # GPU kernel tests (no SDL3 dependency)
        add_executable(test_gpu_kernels frontends/gpu/tests/test_kernels.c)
        target_include_directories(test_gpu_kernels PRIVATE frontends/gpu)
        target_link_libraries(test_gpu_kernels PRIVATE m)
        add_test(NAME gpu_kernel_tests COMMAND test_gpu_kernels)

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

        # JSON preset save/load round-trip + string enum tests
        add_executable(test_preset_json
            frontends/gpu/tests/test_preset_json.c
            frontends/gpu/video_chain.c)
        target_include_directories(test_preset_json PRIVATE frontends/gpu)
        target_link_libraries(test_preset_json PRIVATE m)
        add_test(NAME gpu_preset_json_tests
            COMMAND test_preset_json
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

        # GPU pipeline integration test (headless SDL3 GPU compute)
        add_executable(test_pipeline
            frontends/gpu/tests/test_pipeline.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/video_gpu.c
            frontends/gpu/chroma_pipeline.c
            frontends/gpu/post_pipeline.c
            frontends/gpu/signal_chain.c
            frontends/gpu/video_chain.c
            frontends/gpu/gpu_log.c
        )
        target_include_directories(test_pipeline PRIVATE
            frontends/gpu
            ${SDL3_INCLUDE_DIRS}
        )
        target_link_libraries(test_pipeline PRIVATE nes_static ${SDL3_LIBRARIES} m)
        add_dependencies(test_pipeline gpu_shaders)
        add_test(NAME gpu_pipeline_test COMMAND test_pipeline)

        # GPU image processor — run real pipeline on PNG screenshots
        add_executable(gpu_process
            tools/gpu_process.c
            frontends/gpu/gpu_compute.c
            frontends/gpu/video_gpu.c
            frontends/gpu/chroma_pipeline.c
            frontends/gpu/post_pipeline.c
            frontends/gpu/signal_chain.c
            frontends/gpu/video_chain.c
            frontends/gpu/gpu_log.c
        )
        target_include_directories(gpu_process PRIVATE
            frontends/gpu
            ${SDL3_INCLUDE_DIRS}
        )
        target_link_libraries(gpu_process PRIVATE nes_static ${SDL3_LIBRARIES} m)
        add_dependencies(gpu_process gpu_shaders)
    else()
        message(STATUS "SDL3 not found - skipping GPU frontend (install with: brew install sdl3)")
    endif()
endif()

# =============================================================================
# Installation
# =============================================================================

include(GNUInstallDirs)

install(DIRECTORY include/nes
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS nes_static
    EXPORT NESTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(EXPORT NESTargets
    FILE NESTargets.cmake
    NAMESPACE NES::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NES
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/NESConfig.cmake.in
    ${CMAKE_BINARY_DIR}/NESConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NES
)

write_basic_package_version_file(
    ${CMAKE_BINARY_DIR}/NESConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_BINARY_DIR}/NESConfig.cmake
    ${CMAKE_BINARY_DIR}/NESConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NES
)

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "MyNES Configuration:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build tests:    ${NES_BUILD_TESTS}")
message(STATUS "  Build frontends: ${NES_BUILD_FRONTENDS}")
message(STATUS "  CSI found:      ${CSI_EXECUTABLE}")
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
