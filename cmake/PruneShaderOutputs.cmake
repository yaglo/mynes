# Delete the SPIR-V/MSL files under build/shaders that no current shader
# source produces.
#
# compile_shaders.sh and CopyPrebuiltShaders.cmake only add or overwrite
# files, so the outputs of a renamed or deleted .glsl stayed behind, and the
# release scripts and make-macos-app.sh copy the whole directory.
#
# Invoked by CMakeLists.txt after either of them as:
#   cmake -DSHADER_SRC_DIR=<src> -DSHADER_OUT_DIR=<out> -P PruneShaderOutputs.cmake

# Script mode sets no policy version; IN_LIST etc. need a modern one.
cmake_minimum_required(VERSION 3.16)

if(NOT SHADER_SRC_DIR OR NOT SHADER_OUT_DIR)
    message(FATAL_ERROR "PruneShaderOutputs.cmake needs SHADER_SRC_DIR and SHADER_OUT_DIR")
endif()

foreach(sub compute render)
    # Only stage sources are compiled; fw900_data.glsl and the other includes
    # have no outputs of their own.
    file(GLOB stages RELATIVE "${SHADER_SRC_DIR}/${sub}"
        "${SHADER_SRC_DIR}/${sub}/*.comp.glsl"
        "${SHADER_SRC_DIR}/${sub}/*.vert.glsl"
        "${SHADER_SRC_DIR}/${sub}/*.frag.glsl")
    set(expected "")
    foreach(stage ${stages})
        string(REGEX REPLACE "\\.glsl$" "" stem "${stage}")
        list(APPEND expected "${stem}.spv" "${stem}.msl")
    endforeach()

    file(GLOB built RELATIVE "${SHADER_OUT_DIR}/${sub}"
        "${SHADER_OUT_DIR}/${sub}/*.spv" "${SHADER_OUT_DIR}/${sub}/*.msl")
    foreach(file ${built})
        if(NOT file IN_LIST expected)
            file(REMOVE "${SHADER_OUT_DIR}/${sub}/${file}")
            message(STATUS "Removed ${sub}/${file}: no ${sub}/*.glsl produces it")
        endif()
    endforeach()
endforeach()
