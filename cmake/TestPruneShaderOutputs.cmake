# Test: PruneShaderOutputs.cmake deletes the outputs of shaders whose .glsl
# is gone and keeps those of every current stage source.
#
# Invoked by ctest as:
#   cmake -DWORK_DIR=<scratch dir> -DPRUNE_SCRIPT=<PruneShaderOutputs.cmake> \
#         -P TestPruneShaderOutputs.cmake

# Script mode sets no policy version; IN_LIST etc. need a modern one.
cmake_minimum_required(VERSION 3.16)

if(NOT WORK_DIR OR NOT PRUNE_SCRIPT)
    message(FATAL_ERROR "TestPruneShaderOutputs.cmake needs WORK_DIR and PRUNE_SCRIPT")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
foreach(path
        src/compute/kept.comp.glsl src/compute/included.glsl
        src/render/kept.vert.glsl src/render/kept.frag.glsl
        out/.compile_stamp
        out/compute/kept.comp.spv out/compute/kept.comp.msl
        out/compute/renamed.comp.spv out/compute/renamed.comp.msl
        out/compute/included.spv
        out/render/kept.vert.spv out/render/kept.vert.msl
        out/render/kept.frag.spv out/render/kept.frag.msl
        out/render/removed.frag.spv out/render/removed.frag.msl)
    file(WRITE "${WORK_DIR}/${path}" "")
endforeach()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DSHADER_SRC_DIR=${WORK_DIR}/src
        -DSHADER_OUT_DIR=${WORK_DIR}/out
        -P ${PRUNE_SCRIPT}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "PruneShaderOutputs.cmake exited with ${result}")
endif()

file(GLOB_RECURSE left RELATIVE "${WORK_DIR}/out" "${WORK_DIR}/out/*")
list(SORT left)
set(expected
    .compile_stamp
    compute/kept.comp.msl compute/kept.comp.spv
    render/kept.frag.msl render/kept.frag.spv
    render/kept.vert.msl render/kept.vert.spv)
if(NOT "${left}" STREQUAL "${expected}")
    message(FATAL_ERROR "Pruned output directory\n  expected: ${expected}\n  found:    ${left}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "Stale shader outputs removed, current ones kept")
