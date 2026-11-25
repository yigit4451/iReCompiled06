#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "MarathonCore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MarathonSourceKind
{
    MARATHON_SOURCE_INVALID = 0,
    MARATHON_SOURCE_ISO = 1,
    MARATHON_SOURCE_XEX_DIRECTORY = 2,
    MARATHON_SOURCE_STANDALONE_XEX = 3,
    MARATHON_SOURCE_DLC = 4
} MarathonSourceKind;

/**
 * Progress callback invoked synchronously from StartMarathonExtraction() on the calling worker thread.
 *
 * @param progress        Normalized progress value in [0.0f, 1.0f].
 * @param bytesProcessed  Total bytes extracted/written so far.
 * @param bytesTotal      Expected total bytes to extract.
 * @param statusMessage   UTF-8 phase/status description.
 * @param userData        Opaque caller context pointer.
 * @return true to continue extraction, false to abort and roll back partial files.
 */
typedef bool (*MarathonExtractionProgressCallback)(
    float progress,
    uint64_t bytesProcessed,
    uint64_t bytesTotal,
    const char* statusMessage,
    void* userData
);

/**
 * Checks whether Sonic '06 game files are installed under <Sandbox>/Documents/iReCompiled06/
 * (specifically decrypted/default.bin or game/default.xex).
 */
MARATHON_CORE_API bool IsMarathonInstalled(void);

/**
 * Returns the UTF-8 path to the active installation root (<Sandbox>/Documents/iReCompiled06).
 */
MARATHON_CORE_API const char* GetMarathonInstallDirectory(void);

/**
 * Inspects a coordinated local or cloud FileProvider path (.iso, folder, default.xex, or Xbox 360 DLC)
 * without copying the file. Optionally writes a human-readable summary into detailBuffer.
 */
MARATHON_CORE_API MarathonSourceKind ValidateMarathonSource(
    const char* sourcePath,
    char* detailBuffer,
    int detailBufferSize
);

/**
 * Streams and extracts game or DLC assets directly from a security-scoped / NSFileCoordinator path
 * into <Sandbox>/Documents/iReCompiled06/ without copying the source archive into temporary storage.
 *
 * Call this from a background queue while keeping startAccessingSecurityScopedResource() and
 * NSFileCoordinator.coordinate(readingItemAt:) active for the duration of the call.
 */
MARATHON_CORE_API bool StartMarathonExtraction(
    const char* sourcePath,
    bool skipHashChecks,
    MarathonExtractionProgressCallback progressCallback,
    void* userData,
    char* errorBuffer,
    int errorBufferSize
);

/**
 * Verifies that an existing or manually copied game installation under <Sandbox>/Documents/iReCompiled06/
 * is ready to launch (ensuring decrypted/default.bin or game/default.xex is present and valid).
 */
MARATHON_CORE_API bool VerifyMarathonInstallIntegrity(
    char* detailBuffer,
    int detailBufferSize
);

/**
 * Signals an in-flight StartMarathonExtraction() call to cancel cleanly and roll back partial files.
 */
MARATHON_CORE_API void CancelMarathonExtraction(void);

#ifdef __cplusplus
}
#endif
