#pragma once

#include <filesystem>
#include <list>
#include <string>
#include <string_view>

namespace os::ios
{
    // <Sandbox>/Documents
    // Root user-visible directory (exposed in iOS Files app via UIFileSharingEnabled)
    const std::filesystem::path& GetDocumentsPath();

    // <Sandbox>/Documents/iReCompiled06
    // Game installation root checked by IsMarathonInstalled() and used for game/, decrypted/, dlc/, mods/, save/, config.toml
    const std::filesystem::path& GetGameInstallPath();

    // <Sandbox>/Library/Application Support/iReCompiled06
    // Internal application support directory (excluded from iCloud backup)
    // Used for: pso_caching.txt, marathon.log, registry.ini, MoltenVK/Metal caches
    const std::filesystem::path& GetApplicationSupportPath();

    // <Sandbox>/Library/Caches/iReCompiled06
    const std::filesystem::path& GetCachesPath();

    // <Bundle>.app (read-only bundle resource directory)
    const std::filesystem::path& GetBundleResourcePath();

    // Pre-creates expected iOS Sandbox subdirectories and configures MoltenVK cache env
    void EnsureSandboxDirectories();

    // Opens an external HTTP/HTTPS URL via UIApplication on the main thread
    void OpenURL(std::string_view url);

    // Performs an HTTPS GET request using native Foundation NSURLSession (avoids libcurl/OpenSSL on iOS)
    bool FetchURLText(std::string_view url, std::string_view userAgent, std::string& outBody, std::string& outError);

    // Presents a native UIDocumentPickerViewController for selecting files or folders on iOS
    bool PickFiles(bool folderMode, std::list<std::filesystem::path>& outPaths, std::string& outError);

    // Configures Apple GameController.framework settings for MFi, DualSense, and Xbox controllers on iOS
    void ConfigureGameControllerSupport(bool monitorBackgroundEvents);
}
