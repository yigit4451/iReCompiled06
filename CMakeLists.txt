cmake_minimum_required (VERSION 3.20)

option(MARATHON_RECOMP_HOST_TOOLS_ONLY "Build only native host code-generation tools (for cross-compiling to iOS)" OFF)

if(NOT DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED VCPKG_ROOT AND NOT MARATHON_RECOMP_HOST_TOOLS_ONLY)
    message(FATAL_ERROR "VCPKG_ROOT is not defined!")
endif()

set(MARATHON_RECOMP_THIRDPARTY_ROOT ${CMAKE_SOURCE_DIR}/thirdparty)
set(MARATHON_RECOMP_TOOLS_ROOT ${CMAKE_SOURCE_DIR}/tools)
set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    option(MARATHON_BUILD_IOS_APP "Build the unified SwiftUI iReCompiled06 iOS application bundle" ON)
    if (NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture for iOS" FORCE)
    endif()
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0" CACHE STRING "Minimum iOS deployment target" FORCE)
    endif()
    # Prevent helper executables in subprojects from defaulting to signed .app bundles on iOS.
    set(CMAKE_MACOSX_BUNDLE OFF)
endif()

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_XCODE_GENERATE_SCHEME ON)
endif()

project("MarathonRecomp-ALL")

if (APPLE)
    enable_language(OBJC OBJCXX)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND MARATHON_BUILD_IOS_APP AND CMAKE_GENERATOR STREQUAL "Xcode")
        enable_language(Swift)
    endif()
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(MARATHON_RECOMP_IOS ON)
    add_compile_definitions(MARATHON_RECOMP_IOS=1)
    message(STATUS "Configuring MarathonRecomp for iOS (SDK: ${CMAKE_OSX_SYSROOT}, Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET})")
else()
    set(MARATHON_RECOMP_IOS OFF)
endif()

if (CMAKE_OSX_ARCHITECTURES)
    set(MARATHON_RECOMP_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
elseif(CMAKE_SYSTEM_PROCESSOR)
    set(MARATHON_RECOMP_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
else()
    set(MARATHON_RECOMP_ARCHITECTURE ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${MARATHON_RECOMP_ARCHITECTURE}" MARATHON_RECOMP_ARCHITECTURE)
message(STATUS "Detected architecture: ${MARATHON_RECOMP_ARCHITECTURE}")

if (MARATHON_RECOMP_ARCHITECTURE STREQUAL "x86_64" OR MARATHON_RECOMP_ARCHITECTURE STREQUAL "amd64")
    # Target Sandy Bridge for all projects
    add_compile_options(
        -march=sandybridge
    )
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Normally only defined by Visual Studio, added for consistency
    add_compile_definitions(_DEBUG)
endif()

# Support prebuilt native host tools when cross-compiling (e.g. macOS host -> iOS arm64 target).
set(MARATHON_HOST_TOOLS_DIR "" CACHE PATH "Directory containing native host build tools (file_to_c, XenonRecomp, u8extract, XenosRecomp)")
if (MARATHON_HOST_TOOLS_DIR)
    find_program(MARATHON_RECOMP_HOST_FILE_TO_C NAMES file_to_c
        PATHS "${MARATHON_HOST_TOOLS_DIR}" "${MARATHON_HOST_TOOLS_DIR}/tools/file_to_c" NO_DEFAULT_PATH)
    find_program(MARATHON_RECOMP_HOST_XENON_RECOMP NAMES XenonRecomp
        PATHS "${MARATHON_HOST_TOOLS_DIR}" "${MARATHON_HOST_TOOLS_DIR}/tools/XenonRecomp/XenonRecomp" "${MARATHON_HOST_TOOLS_DIR}/tools/XenonRecomp" NO_DEFAULT_PATH)
    find_program(MARATHON_RECOMP_HOST_U8EXTRACT NAMES u8extract
        PATHS "${MARATHON_HOST_TOOLS_DIR}" "${MARATHON_HOST_TOOLS_DIR}/tools/u8extract" NO_DEFAULT_PATH)
    find_program(MARATHON_RECOMP_HOST_XENOS_RECOMP NAMES XenosRecomp
        PATHS "${MARATHON_HOST_TOOLS_DIR}" "${MARATHON_HOST_TOOLS_DIR}/tools/XenosRecomp/XenosRecomp" "${MARATHON_HOST_TOOLS_DIR}/tools/XenosRecomp" NO_DEFAULT_PATH)
endif()

if (MARATHON_RECOMP_HOST_XENON_RECOMP AND MARATHON_RECOMP_HOST_XENOS_RECOMP AND MARATHON_RECOMP_HOST_FILE_TO_C AND MARATHON_RECOMP_HOST_U8EXTRACT)
    set(MARATHON_RECOMP_USE_PREBUILT_HOST_TOOLS ON)
else()
    set(MARATHON_RECOMP_USE_PREBUILT_HOST_TOOLS OFF)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT MARATHON_RECOMP_HOST_TOOLS_ONLY)
        message(WARNING "Cross-compiling for iOS without MARATHON_HOST_TOOLS_DIR set: build native macOS host tools first (-DMARATHON_RECOMP_HOST_TOOLS_ONLY=ON) and pass -DMARATHON_HOST_TOOLS_DIR=<path>.")
    endif()
endif()

if (NOT MARATHON_RECOMP_HOST_TOOLS_ONLY)
    add_subdirectory(${MARATHON_RECOMP_THIRDPARTY_ROOT})
endif()

add_subdirectory(${MARATHON_RECOMP_TOOLS_ROOT})

if (NOT MARATHON_RECOMP_HOST_TOOLS_ONLY)
    # Include sub-projects.
    add_subdirectory("MarathonRecompLib")
    add_subdirectory("MarathonRecomp")
endif()
