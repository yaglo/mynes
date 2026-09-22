# Test: the shader directory the frontend loads from must hold exactly the
# files that are committed under frontends/gpu/shaders.
#
# Both build paths (compiling GLSL, or copying the committed SPIR-V/MSL when
# the tools are missing) have to produce the same layout, and a new .glsl
# whose outputs were not committed would otherwise only break machines
# without the tool chain.
#
# Invoked by ctest as:
#   cmake -DSHADER_SRC_DIR=<src> -DSHADER_OUT_DIR=<out> -P CheckShaderLayout.cmake

# Script mode sets no policy version; IN_LIST etc. need a modern one.
cmake_minimum_required(VERSION 3.16)

if(NOT SHADER_SRC_DIR OR NOT SHADER_OUT_DIR)
    message(FATAL_ERROR "CheckShaderLayout.cmake needs SHADER_SRC_DIR and SHADER_OUT_DIR")
endif()

set(failed FALSE)
foreach(sub compute render)
    file(GLOB sources RELATIVE "${SHADER_SRC_DIR}/${sub}" "${SHADER_SRC_DIR}/${sub}/*.glsl")
    file(GLOB committed RELATIVE "${SHADER_SRC_DIR}/${sub}"
        "${SHADER_SRC_DIR}/${sub}/*.spv" "${SHADER_SRC_DIR}/${sub}/*.msl")
    file(GLOB built RELATIVE "${SHADER_OUT_DIR}/${sub}"
        "${SHADER_OUT_DIR}/${sub}/*.spv" "${SHADER_OUT_DIR}/${sub}/*.msl")

    # Every stage source (*.comp/*.vert/*.frag) needs a committed .spv and .msl.
    # Plain .glsl includes (fw900_data.glsl etc.) are not compiled on their own.
    foreach(src ${sources})
        if(src MATCHES "\\.(comp|vert|frag)\\.glsl$")
            string(REGEX REPLACE "\\.glsl$" "" stem "${src}")
            foreach(ext spv msl)
                if(NOT "${stem}.${ext}" IN_LIST committed)
                    message(SEND_ERROR "${sub}/${stem}.${ext} is not committed; run the shaders_regenerate target")
                    set(failed TRUE)
                endif()
            endforeach()
        endif()
    endforeach()

    list(SORT committed)
    list(SORT built)
    if(NOT "${committed}" STREQUAL "${built}")
        message(SEND_ERROR
            "Shader layout mismatch in ${sub}/\n"
            "  committed: ${committed}\n"
            "  built:     ${built}")
        set(failed TRUE)
    endif()
    if(NOT failed)
        list(LENGTH built n)
        message(STATUS "${sub}: ${n} shader files match the committed layout")
    endif()
endforeach()

if(failed)
    message(FATAL_ERROR "Shader layout check failed")
endif()
