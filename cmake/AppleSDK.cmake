# Resolve the SDK from the selected developer tools before project() probes the
# compiler. A cached xcrun SDK can still point at CommandLineTools after switching
# to Xcode, whose linker may not understand that SDK's newer .tbd format.
# Explicit toolchain, SDKROOT, and -DCMAKE_OSX_SYSROOT selections take precedence.
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND
   NOT DEFINED CMAKE_OSX_SYSROOT AND "$ENV{SDKROOT}" STREQUAL "" AND
   (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    execute_process(
        COMMAND /usr/bin/xcrun --no-cache --sdk macosx --show-sdk-path
        RESULT_VARIABLE _mynes_sdk_result
        OUTPUT_VARIABLE _mynes_sdk_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_mynes_sdk_result EQUAL 0 AND IS_DIRECTORY "${_mynes_sdk_path}")
        set(CMAKE_OSX_SYSROOT "${_mynes_sdk_path}" CACHE PATH "macOS SDK")
    endif()
    unset(_mynes_sdk_result)
    unset(_mynes_sdk_path)
endif()

# Local builds use libraries installed for the running OS (e.g. Homebrew SDL).
# Do not inherit an older minimum OS merely because Xcode ships an older SDK.
# Release builds can explicitly select a lower deployment target as before.
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND
   NOT CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "" AND
   (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    execute_process(
        COMMAND /usr/bin/sw_vers -productVersion
        RESULT_VARIABLE _mynes_os_result
        OUTPUT_VARIABLE _mynes_os_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_mynes_os_result EQUAL 0 AND _mynes_os_version MATCHES "^[0-9]+\\.[0-9]+")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_MATCH_0}" CACHE STRING
            "Minimum macOS version (defaults to the local OS)" FORCE)
    endif()
    unset(_mynes_os_result)
    unset(_mynes_os_version)
endif()
