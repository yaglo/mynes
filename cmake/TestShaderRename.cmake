# Test: after a shader is renamed with its mtime kept, as git mv does, the
# next build fills build/shaders with the new names and no old ones.
#
# Builds a scratch project whose shader step comes from ShaderStep.cmake,
# on the copy path, over a small shader tree. The rename leaves every input
# older than the stamp, so only the step's list of shader files makes the
# build run it again.
#
# Invoked by ctest as:
#   cmake -DWORK_DIR=<scratch dir> -DMODULE_DIR=<source>/cmake \
#         -DGENERATOR=<generator> [-DMAKE_PROGRAM=<program>] \
#         -P TestShaderRename.cmake

# Script mode sets no policy version; IN_LIST etc. need a modern one.
cmake_minimum_required(VERSION 3.16)

if(NOT WORK_DIR OR NOT MODULE_DIR OR NOT GENERATOR)
    message(FATAL_ERROR "TestShaderRename.cmake needs WORK_DIR, MODULE_DIR and GENERATOR")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
foreach(path
        compute/kept.comp compute/included render/kept.vert render/old.frag)
    file(WRITE "${WORK_DIR}/src/${path}.glsl" "")
    if(NOT path MATCHES "included")
        file(WRITE "${WORK_DIR}/src/${path}.spv" "")
        file(WRITE "${WORK_DIR}/src/${path}.msl" "")
    endif()
endforeach()

file(WRITE "${WORK_DIR}/project/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.16)
project(shader_rename_test NONE)
include(\"${MODULE_DIR}/ShaderStep.cmake\")
set(out \"\${CMAKE_BINARY_DIR}/shaders\")
mynes_shader_step(
    SRC_DIR \"${WORK_DIR}/src\" OUT_DIR \"\${out}\" STAMP \"\${out}/.compile_stamp\"
    FROM COMMITTED
    COMMENT \"Copying shaders\"
    COMMAND \"\${CMAKE_COMMAND}\"
        \"-DSHADER_SRC_DIR=${WORK_DIR}/src\"
        \"-DSHADER_OUT_DIR=\${out}\"
        -P \"${MODULE_DIR}/CopyPrebuiltShaders.cmake\"
    DEPENDS \"${MODULE_DIR}/CopyPrebuiltShaders.cmake\")
add_custom_target(shaders ALL DEPENDS \"\${out}/.compile_stamp\")
")

function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${what} failed with ${result}:\n${output}")
    endif()
endfunction()

function(check_layout when)
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -DSHADER_SRC_DIR=${WORK_DIR}/src
            -DSHADER_OUT_DIR=${WORK_DIR}/build/shaders
            -P ${MODULE_DIR}/CheckShaderLayout.cmake
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Shader layout ${when}:\n${output}")
    endif()
endfunction()

set(configure ${CMAKE_COMMAND} -S ${WORK_DIR}/project -B ${WORK_DIR}/build -G ${GENERATOR})
if(MAKE_PROGRAM)
    list(APPEND configure -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()
run("Configuring the scratch project" ${configure})
run("The first build" ${CMAKE_COMMAND} --build ${WORK_DIR}/build)
check_layout("after the first build")

# Make the stamp at least a second older than anything the next build
# writes, for file systems that keep whole seconds.
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
# file(RENAME) keeps the mtime.
foreach(ext glsl spv msl)
    file(RENAME "${WORK_DIR}/src/render/old.frag.${ext}" "${WORK_DIR}/src/render/new.frag.${ext}")
endforeach()
run("The build after the rename" ${CMAKE_COMMAND} --build ${WORK_DIR}/build)
check_layout("after the rename")

file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "A renamed shader was rebuilt under its new name only")
