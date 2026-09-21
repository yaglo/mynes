# Verify both the pristine input and final patched file; this is also safe to
# rerun and catches silently skipped patches inside a parent Git worktree.
set(metal "${SDL_SOURCE}/src/gpu/metal/SDL_gpu_metal.m")
set(pristine 0c82b0527740178cabed21c3dc6dd289fa8515fbe697fea5c735b14147cebfdb)
set(patched 046630f8221e2e57a39215cc1b93e97e53bc4f660b9b74e88ddcddbc3f3c89ee)
file(SHA256 "${metal}" actual)
if(actual STREQUAL patched)
    return()
endif()
if(NOT actual STREQUAL pristine)
    message(FATAL_ERROR "Unexpected SDL Metal source; use the pinned pristine or patched source")
endif()
# Disable parent-repository discovery: git apply under build/_deps otherwise
# can silently skip paths relative to the MyNES repository root.
foreach(patch sdl-metal-fence-return sdl-metal-fence-lifetime mynes-metal-presentation)
    execute_process(COMMAND "${GIT_EXECUTABLE}"
        "--git-dir=${SDL_SOURCE}/.mynes-patch-no-repository" apply
        "${CMAKE_CURRENT_LIST_DIR}/patches/${patch}.patch"
        WORKING_DIRECTORY "${SDL_SOURCE}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not apply SDL patch: ${patch}")
    endif()
endforeach()
file(SHA256 "${metal}" actual)
if(NOT actual STREQUAL patched)
    message(FATAL_ERROR "SDL Metal patch verification failed")
endif()
