# The custom command that fills build/shaders. CMakeLists.txt uses it for
# both the compile and the copy path, and TestShaderRename.cmake builds a
# scratch project with it to check that renaming a shader rebuilds.
#
# mynes_shader_step(SRC_DIR <dir> OUT_DIR <dir> STAMP <file>
#                   FROM GLSL|COMMITTED COMMENT <text>
#                   COMMAND <command...> [DEPENDS <file...>])
#
# The step runs COMMAND, which writes <OUT_DIR>/compute and render, then
# PruneShaderOutputs.cmake, and touches STAMP. It depends on the .glsl files
# under SRC_DIR (FROM GLSL) or on the committed .spv/.msl (FROM COMMITTED),
# on DEPENDS and on a list of all of these files.
#
# Renaming or deleting a shader leaves no input newer than the stamp (git mv
# keeps the file's mtime), so without the list the step would not rerun:
# build/shaders would keep the old outputs and lack the new ones. The globs
# below are CONFIGURE_DEPENDS, so a build that finds a different set of
# files reconfigures, and the list is rewritten only when the set changed.
# It is kept in CMakeFiles/ outside build/shaders so that deleting that
# directory still rebuilds it.

set(MYNES_SHADER_STEP_DIR ${CMAKE_CURRENT_LIST_DIR})

function(mynes_shader_step)
    cmake_parse_arguments(PARSE_ARGV 0 arg ""
        "SRC_DIR;OUT_DIR;STAMP;FROM;COMMENT" "COMMAND;DEPENDS")
    # The .glsl globs include the compute includes such as fw900_data.glsl.
    file(GLOB glsl CONFIGURE_DEPENDS
        ${arg_SRC_DIR}/compute/*.glsl ${arg_SRC_DIR}/render/*.glsl)
    file(GLOB committed CONFIGURE_DEPENDS
        ${arg_SRC_DIR}/compute/*.spv ${arg_SRC_DIR}/compute/*.msl
        ${arg_SRC_DIR}/render/*.spv ${arg_SRC_DIR}/render/*.msl)
    if(arg_FROM STREQUAL "GLSL")
        set(inputs ${glsl})
    elseif(arg_FROM STREQUAL "COMMITTED")
        set(inputs ${committed})
    else()
        message(FATAL_ERROR "mynes_shader_step: FROM must be GLSL or COMMITTED")
    endif()

    set(list_file ${CMAKE_BINARY_DIR}/CMakeFiles/mynes_shader_files.txt)
    string(REPLACE ";" "\n" text "${glsl};${committed}\n")
    set(old "")
    if(EXISTS ${list_file})
        file(READ ${list_file} old)
    endif()
    if(NOT old STREQUAL text)
        file(WRITE ${list_file} "${text}")
    endif()

    set(prune ${MYNES_SHADER_STEP_DIR}/PruneShaderOutputs.cmake)
    add_custom_command(
        OUTPUT ${arg_STAMP}
        COMMAND ${arg_COMMAND}
        COMMAND ${CMAKE_COMMAND}
            -DSHADER_SRC_DIR=${arg_SRC_DIR}
            -DSHADER_OUT_DIR=${arg_OUT_DIR}
            -P ${prune}
        COMMAND ${CMAKE_COMMAND} -E touch ${arg_STAMP}
        DEPENDS ${inputs} ${arg_DEPENDS} ${list_file} ${prune}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "${arg_COMMENT}"
        VERBATIM
    )
endfunction()
