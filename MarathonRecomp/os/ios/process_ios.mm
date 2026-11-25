#include <os/process.h>
#include <os/logger.h>
#include <os/ios/sandbox_ios.h>

#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <sys/param.h>
#include <unistd.h>

std::filesystem::path os::process::GetExecutablePath()
{
    @autoreleasepool
    {
        NSString* exePathStr = [[NSBundle mainBundle] executablePath];
        if (exePathStr && exePathStr.length > 0)
            return std::filesystem::path([exePathStr fileSystemRepresentation]);
    }

    uint32_t exePathSize = PATH_MAX;
    char exePath[PATH_MAX] = {};
    if (_NSGetExecutablePath(exePath, &exePathSize) == 0)
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(exePath)));

    return {};
}

std::filesystem::path os::process::GetExecutableRoot()
{
    // On iOS, the .app bundle and its parent container directory are read-only / sandboxed.
    // Route executable root lookups into the writable Sandbox Documents directory.
    return os::ios::GetDocumentsPath();
}

std::filesystem::path os::process::GetWorkingDirectory()
{
    char cwd[PATH_MAX] = {};
    if (getcwd(cwd, sizeof(cwd)) != nullptr)
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(cwd)));

    return os::ios::GetDocumentsPath();
}

bool os::process::SetWorkingDirectory(const std::filesystem::path& path)
{
    return chdir(path.c_str()) == 0;
}

bool os::process::StartProcess(const std::filesystem::path& path, const std::vector<std::string>& args, std::filesystem::path work)
{
    // Spawning child processes via fork()/execvp() is prohibited inside the iOS App Sandbox.
    // Persist requested restart arguments in NSUserDefaults so the SwiftUI host can inspect them on next launch.
    @autoreleasepool
    {
        NSMutableArray<NSString*>* nsArgs = [NSMutableArray arrayWithCapacity:args.size()];
        for (const auto& arg : args)
        {
            NSString* nsArg = [NSString stringWithUTF8String:arg.c_str()];
            if (nsArg)
                [nsArgs addObject:nsArg];
        }
        [[NSUserDefaults standardUserDefaults] setObject:nsArgs forKey:@"MarathonPendingRestartArgs"];
        [[NSUserDefaults standardUserDefaults] synchronize];
    }

    LOGFN_WARNING("StartProcess is unavailable in iOS Sandbox (requested target: {}).", path.string());
    return false;
}

void os::process::CheckConsole()
{
    g_consoleVisible = true;
}

void os::process::ShowConsole()
{
    // Handled via Unified Logging (os_log) and marathon.log on iOS.
}
