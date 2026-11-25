#include <os/logger.h>
#include <os/ios/sandbox_ios.h>

#import <Foundation/Foundation.h>
#include <os/log.h>
#include <fstream>
#include <mutex>

namespace
{
    os_log_t g_iosLog = nullptr;
    std::ofstream g_logFile;
    std::mutex g_logMutex;
}

void os::logger::Init()
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_iosLog)
        g_iosLog = os_log_create("sonicnext-dev.MarathonRecomp", "MarathonCore");

    if (!g_logFile.is_open())
    {
        const auto logPath = os::ios::GetApplicationSupportPath() / "marathon.log";
        g_logFile.open(logPath, std::ios::out | std::ios::trunc);
    }
}

void os::logger::Log(const std::string_view str, ELogType type, const char* func)
{
    std::string formatted = func
        ? fmt::format("[{}] {}", func, str)
        : std::string(str);

    os_log_type_t logType = OS_LOG_TYPE_DEFAULT;
    switch (type)
    {
        case ELogType::Info:
            logType = OS_LOG_TYPE_INFO;
            break;
        case ELogType::Utility:
            logType = OS_LOG_TYPE_DEBUG;
            break;
        case ELogType::Warning:
            logType = OS_LOG_TYPE_DEFAULT;
            break;
        case ELogType::Error:
            logType = OS_LOG_TYPE_ERROR;
            break;
    }

    if (g_iosLog)
        os_log_with_type(g_iosLog, logType, "%{public}s", formatted.c_str());
    else
        fmt::println("{}", formatted);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open())
    {
        g_logFile << formatted << '\n';
        g_logFile.flush();
    }
}
