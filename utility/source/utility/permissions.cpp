#include <utility/permissions.hpp>

#include <fstream>
#include <string>

#ifndef _WIN32
#    include <sys/stat.h>
#endif

namespace Utility
{
    namespace
    {
        std::filesystem::perms readProcessUmask()
        {
#ifdef _WIN32
            return std::filesystem::perms::none;
#else
#    ifdef __linux__
            // Reading /proc leaves the mask alone, changing it would race other threads creating files.
            std::ifstream status{"/proc/self/status"};
            std::string line;
            while (std::getline(status, line))
            {
                if (line.starts_with("Umask:"))
                    return static_cast<std::filesystem::perms>(std::stoul(line.substr(6), nullptr, 8));
            }
#    endif
            const auto mask = ::umask(0);
            ::umask(mask);
            return static_cast<std::filesystem::perms>(mask);
#endif
        }
    }

    std::optional<std::filesystem::perms> sanitizePermissions(std::filesystem::perms permissions)
    {
        if (permissions == std::filesystem::perms::unknown)
            return std::nullopt;
        return permissions & std::filesystem::perms::all;
    }

    std::filesystem::perms processUmask()
    {
        static const auto mask = readProcessUmask();
        return mask;
    }

    std::optional<std::filesystem::perms> inheritablePermissions(std::filesystem::perms permissions)
    {
        const auto sanitized = sanitizePermissions(permissions);
        if (!sanitized)
            return std::nullopt;
        return *sanitized & ~processUmask();
    }

    std::filesystem::perms localPermissions(std::filesystem::file_status const& status)
    {
#ifdef _WIN32
        static_cast<void>(status);
        return std::filesystem::perms::unknown;
#else
        return status.permissions();
#endif
    }
}
