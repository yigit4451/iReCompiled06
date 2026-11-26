#pragma once

#include <stdbool.h>

#if defined(_WIN32)
    #if defined(MARATHON_CORE_EXPORTS)
        #define MARATHON_CORE_API __declspec(dllexport)
    #elif defined(MARATHON_CORE_SHARED)
        #define MARATHON_CORE_API __declspec(dllimport)
    #else
        #define MARATHON_CORE_API
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #define MARATHON_CORE_API __attribute__((visibility("default")))
#else
    #define MARATHON_CORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Entry point for running MarathonRecomp from an external host (e.g. SwiftUI on iOS).
 * Note: On iOS (UIKit/SDL3), invoke on the main thread after the UIApplication lifecycle starts.
 */
MARATHON_CORE_API int MarathonMain(int argc, char** argv);

/**
 * Returns the UTF-8 path to the user-accessible Documents directory
 * (used on iOS for game files, DLC, mods, saves, and config.toml).
 */
MARATHON_CORE_API const char* MarathonGetDocumentsPath(void);

/**
 * Returns the UTF-8 path to the internal Application Support directory
 * (used on iOS for shader/PSO caches, logs, and internal state).
 */
MARATHON_CORE_API const char* MarathonGetApplicationSupportPath(void);

/**
 * Checks whether valid game files are currently installed in the active game directory.
 */
MARATHON_CORE_API bool MarathonIsGameInstalled(void);

/**
 * Returns the last fatal guest/engine diagnostic message (UTF-8, empty when none),
 * e.g. the missing guest function address report produced by GuestThread::Start.
 */
MARATHON_CORE_API const char* MarathonGetLastErrorMessage(void);

#ifdef __cplusplus
}
#endif
