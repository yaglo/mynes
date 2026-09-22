# Verify both the pristine input and final patched file; this is also safe to
# rerun and catches silently skipped patches inside a parent Git worktree.
set(metal "${SDL_SOURCE}/src/gpu/metal/SDL_gpu_metal.m")
set(pristine 0c82b0527740178cabed21c3dc6dd289fa8515fbe697fea5c735b14147cebfdb)
set(previous 046630f8221e2e57a39215cc1b93e97e53bc4f660b9b74e88ddcddbc3f3c89ee)
set(patched a93eeaacc8b3e8d0316e4591e2dd4fb854dcb4eb3d757a2ce0e1d65668793481)
# Also refresh this shared, unit-tested helper for cached/offline SDL sources.
configure_file("${CMAKE_CURRENT_LIST_DIR}/../frontends/gpu/presentation_schedule.h"
    "${SDL_SOURCE}/src/gpu/metal/presentation_schedule.h" COPYONLY)
file(SHA256 "${metal}" actual)
if(actual STREQUAL patched)
    return()
endif()
if(NOT actual STREQUAL pristine AND NOT actual STREQUAL previous)
    message(FATAL_ERROR "Unexpected SDL Metal source; use the pinned pristine or patched source")
endif()
# Disable parent-repository discovery: git apply under build/_deps otherwise
# can silently skip paths relative to the MyNES repository root.
set(patches mynes-metal-presentation-recovery)
if(actual STREQUAL pristine)
    list(PREPEND patches sdl-metal-fence-return sdl-metal-fence-lifetime mynes-metal-presentation)
endif()
foreach(patch IN LISTS patches)
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
