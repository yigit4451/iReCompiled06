#include <os/registry.h>
#include <os/ios/sandbox_ios.h>

#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace os::registry::detail
{
    inline std::filesystem::path GetRegistryFilePath()
    {
        return os::ios::GetApplicationSupportPath() / "registry.ini";
    }

    inline std::unordered_map<std::string, std::string> LoadEntries()
    {
        std::unordered_map<std::string, std::string> entries;
        std::ifstream file(GetRegistryFilePath());
        if (!file.is_open())
            return entries;

        std::string line;
        while (std::getline(file, line))
        {
            const auto pos = line.find('=');
            if (pos != std::string::npos)
                entries[line.substr(0, pos)] = line.substr(pos + 1);
        }
        return entries;
    }

    inline bool SaveEntries(const std::unordered_map<std::string, std::string>& entries)
    {
        std::ofstream file(GetRegistryFilePath(), std::ios::out | std::ios::trunc);
        if (!file.is_open())
            return false;

        for (const auto& [k, v] : entries)
            file << k << '=' << v << '\n';

        return true;
    }
}

inline bool os::registry::Init()
{
    std::error_code ec;
    const auto dir = os::ios::GetApplicationSupportPath();
    std::filesystem::create_directories(dir, ec);
    return !dir.empty() && !ec;
}

template<typename T>
bool os::registry::ReadValue(const std::string_view& name, T& data)
{
    auto entries = detail::LoadEntries();
    auto it = entries.find(std::string(name));
    if (it == entries.end())
        return false;

    if constexpr (std::is_same_v<T, std::filesystem::path>)
    {
        data = std::filesystem::path(it->second);
        return true;
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        data = it->second;
        return true;
    }
    else
    {
        std::istringstream iss(it->second);
        iss >> data;
        return !iss.fail();
    }
}

template<typename T>
bool os::registry::WriteValue(const std::string_view& name, const T& data)
{
    auto entries = detail::LoadEntries();

    if constexpr (std::is_same_v<T, std::filesystem::path>)
    {
        entries[std::string(name)] = data.string();
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        entries[std::string(name)] = data;
    }
    else
    {
        std::ostringstream oss;
        oss << data;
        entries[std::string(name)] = oss.str();
    }

    return detail::SaveEntries(entries);
}
