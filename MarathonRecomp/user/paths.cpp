#include "paths.h"
#include <os/process.h>

std::filesystem::path g_executableRoot = os::process::GetExecutableRoot();
std::filesystem::path g_userPath = BuildUserPath();

bool CheckPortable()
{
#if defined(__APPLE__) && TARGET_OS_IOS
    return false;
#else
    std::error_code ec;
    return std::filesystem::exists(g_executableRoot / "portable.txt", ec);
#endif
}

std::filesystem::path BuildUserPath()
{
#if defined(__APPLE__) && TARGET_OS_IOS
    // On iOS, user-accessible data (saves, mods, config.toml) lives in <Sandbox>/Documents/iReCompiled06.
    return os::ios::GetGameInstallPath();
#else
    if (CheckPortable())
        return g_executableRoot;

    std::filesystem::path userPath;

#if defined(_WIN32)
    PWSTR knownPath = NULL;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &knownPath) == S_OK)
        userPath = std::filesystem::path{ knownPath } / USER_DIRECTORY;

    CoTaskMemFree(knownPath);
#elif defined(__linux__) || defined(__APPLE__)
    const char* homeDir = getenv("HOME");
#if defined(__linux__)
    if (homeDir == nullptr)
    {
        homeDir = getpwuid(getuid())->pw_dir;
    }
#endif

    if (homeDir != nullptr)
    {
        // Prefer to store in the .config directory if it exists. Use the home directory otherwise.
        std::filesystem::path homePath = homeDir;
#if defined(__linux__)
        const char* configDir = getenv("XDG_CONFIG_HOME");
        std::filesystem::path configPath = "";
        if (configDir == nullptr)
        {
            configPath = homePath / ".config";
        } else
        {
            configPath = configDir;
        }

#else
        std::filesystem::path configPath = homePath / "Library" / "Application Support";
#endif
        if (std::filesystem::exists(configPath))
            userPath = configPath / USER_DIRECTORY;
        else
            userPath = homePath / ("." USER_DIRECTORY);
    }
#else
    static_assert(false, "GetUserPath() not implemented for this platform.");
#endif

    return userPath;
#endif
}

const std::filesystem::path& GetUserPath()
{
    return g_userPath;
}

const std::filesystem::path& GetAppSupportPath()
{
#if defined(__APPLE__) && TARGET_OS_IOS
    // Internal cache and support files live in <Sandbox>/Library/Application Support/MarathonRecomp on iOS.
    return os::ios::GetApplicationSupportPath();
#else
    return g_userPath;
#endif
}
