cmake_policy(SET CMP0077 NEW)

set(MSDF_ATLAS_BUILD_STANDALONE OFF)
set(MSDF_ATLAS_USE_SKIA OFF)
set(MSDF_ATLAS_NO_ARTERY_FONT ON)
set(MSDFGEN_DISABLE_PNG ON)

set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED OFF)
set(SDLMIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VENDORED ON)
set(SDLMIXER_FLAC OFF)
set(SDLMIXER_MOD OFF)
set(SDLMIXER_MP3 OFF)
set(SDLMIXER_MIDI OFF)
set(SDLMIXER_OPUS OFF)
set(SDLMIXER_VORBIS_STB OFF)
set(SDLMIXER_WAVPACK OFF)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(PLUME_SDL3_VULKAN_ENABLED ON CACHE BOOL "")
endif()

if (WIN32)
    set(PLUME_D3D12_AGILITY_SDK_ENABLED ON CACHE BOOL "")
endif()

if (APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(BASE_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
elseif (CMAKE_SYSTEM_PROCESSOR)
    set(BASE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(BASE_ARCHITECTURE "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

# Next, match common architecture strings down to a known common value.
if (BASE_ARCHITECTURE MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set(ARCHITECTURE "x86_64")
elseif (BASE_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
    set(ARCHITECTURE "arm64")
else()
    message(FATAL_ERROR "Unsupported CPU architecture: ${BASE_ARCHITECTURE}")
endif()

add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/msdf-atlas-gen")
if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # nativefiledialog-extended relies on macOS AppKit under APPLE; iOS uses UIDocumentPickerViewController in os/ios/sandbox_ios.mm.
    add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/nativefiledialog-extended")
endif()
add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/o1heap")
add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/SDL")
add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/SDL_mixer")
add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/plume")

if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET plume)
    # Replace macOS AppKit/IOKit plume_apple.mm with iOS UIKit implementation while keeping thirdparty/plume submodule clean.
    get_target_property(_PLUME_SOURCES plume SOURCES)
    if (_PLUME_SOURCES)
        list(FILTER _PLUME_SOURCES EXCLUDE REGEX "plume_apple\\.mm$")
        list(APPEND _PLUME_SOURCES "${CMAKE_SOURCE_DIR}/MarathonRecomp/os/ios/plume_ios.mm")
        set_target_properties(plume PROPERTIES SOURCES "${_PLUME_SOURCES}")
    endif()
    target_include_directories(plume PRIVATE "${MARATHON_RECOMP_THIRDPARTY_ROOT}/plume")
endif()

set(MARATHON_IOS_FFMPEG_ROOT "" CACHE PATH "Optional root directory containing prebuilt iOS arm64 FFmpeg include/ and lib/")
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND MARATHON_IOS_FFMPEG_ROOT)
    add_library(ffmpeg INTERFACE)
    target_include_directories(ffmpeg INTERFACE "${MARATHON_IOS_FFMPEG_ROOT}/include")
    target_link_libraries(ffmpeg INTERFACE
        "-framework CoreFoundation"
        "-framework AudioToolbox"
        "-framework CoreMedia"
        "-framework VideoToolbox"
        "-framework CoreVideo"
        "-framework Security"
        "${MARATHON_IOS_FFMPEG_ROOT}/lib/libavformat.a"
        "${MARATHON_IOS_FFMPEG_ROOT}/lib/libavcodec.a"
        "${MARATHON_IOS_FFMPEG_ROOT}/lib/libswscale.a"
        "${MARATHON_IOS_FFMPEG_ROOT}/lib/libavutil.a"
        "${MARATHON_IOS_FFMPEG_ROOT}/lib/libavfilter.a"
        "${MARATHON_IOS_FFMPEG_ROOT}/lib/libswresample.a"
        z
    )
else()
    add_subdirectory("${MARATHON_RECOMP_THIRDPARTY_ROOT}/ffmpeg-core")
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET ffmpeg)
        # Strip macOS-only umbrella frameworks (AudioUnit/CoreServices/CoreAudio standalone) on iOS SDK.
        get_target_property(_FFMPEG_LIBS ffmpeg INTERFACE_LINK_LIBRARIES)
        if (_FFMPEG_LIBS)
            list(REMOVE_ITEM _FFMPEG_LIBS "-framework CoreServices" "-framework AudioUnit")
            set_target_properties(ffmpeg PROPERTIES INTERFACE_LINK_LIBRARIES "${_FFMPEG_LIBS}")
        endif()
    endif()
endif()
