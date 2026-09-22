# Populate build/shaders from the committed SPIR-V/MSL files.
#
# Used when glslc, spirv-cross or python3 is missing, so that a checkout
# builds without the shader tool chain. The layout must match what
# compile_shaders.sh writes (<out>/compute/*.spv|*.msl, <out>/render/...)
# because the frontend resolves shaders relative to its executable.
#
# Invoked by CMakeLists.txt as:
#   cmake -DSHADER_SRC_DIR=<src> -DSHADER_OUT_DIR=<out> -P CopyPrebuiltShaders.cmake

# Script mode sets no policy version; IN_LIST etc. need a modern one.
cmake_minimum_required(VERSION 3.16)

if(NOT SHADER_SRC_DIR OR NOT SHADER_OUT_DIR)
    message(FATAL_ERROR "CopyPrebuiltShaders.cmake needs SHADER_SRC_DIR and SHADER_OUT_DIR")
endif()

set(copied 0)
foreach(sub compute render)
    file(GLOB files
        "${SHADER_SRC_DIR}/${sub}/*.spv"
        "${SHADER_SRC_DIR}/${sub}/*.msl")
    if(NOT files)
        message(FATAL_ERROR
            "No committed shaders under ${SHADER_SRC_DIR}/${sub}; "
            "install glslc (shaderc) and spirv-cross to compile them")
    endif()
    file(MAKE_DIRECTORY "${SHADER_OUT_DIR}/${sub}")
    # file(COPY) keeps timestamps and skips files that are already current.
    file(COPY ${files} DESTINATION "${SHADER_OUT_DIR}/${sub}")
    list(LENGTH files n)
    math(EXPR copied "${copied} + ${n}")
endforeach()
message(STATUS "Copied ${copied} committed shader files to ${SHADER_OUT_DIR}")
