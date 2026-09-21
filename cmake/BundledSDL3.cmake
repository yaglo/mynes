# Stable SDL plus the upstream fixes for concurrent audio/render fence use.
# Static linking makes packaged/copied executables independent of Homebrew.
include(FetchContent)
find_package(Git REQUIRED)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mynes_sdl3_source
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSDL_SOURCE=<SOURCE_DIR>
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_LIST_DIR}/PatchSDL3.cmake)
FetchContent_MakeAvailable(mynes_sdl3_source)
# Verify cached/offline source overrides too, which bypass PATCH_COMMAND.
set(SDL_SOURCE "${mynes_sdl3_source_SOURCE_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/PatchSDL3.cmake")
