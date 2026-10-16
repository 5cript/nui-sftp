#include <utility/download_mark.hpp>

#ifdef _WIN32
#    include <fstream>
#elif defined(__APPLE__)
#    include <sys/xattr.h>

#    include <cerrno>
#    include <chrono>
#    include <cstdio>
#    include <string>
#endif

namespace Utility
{
    std::error_code markAsDownloaded(std::filesystem::path const& path)
    {
#ifdef _WIN32
        auto streamPath = path;
        streamPath += L":Zone.Identifier";
        std::ofstream stream{streamPath, std::ios::binary | std::ios::trunc};
        if (!stream)
            return std::make_error_code(std::errc::operation_not_supported);
        // Zone 3 is the internet zone, SmartScreen and Office's protected view apply.
        stream << "[ZoneTransfer]\r\nZoneId=3\r\n";
        if (!stream.flush())
            return std::make_error_code(std::errc::io_error);
        return {};
#elif defined(__APPLE__)
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        char value[64]{};
        // Flags 0081: downloaded, not yet approved by the user; then the time in hex and the downloading agent.
        const auto length = std::snprintf(value, sizeof(value), "0081;%llx;nui-sftp;", static_cast<unsigned long long>(seconds));
        if (::setxattr(path.c_str(), "com.apple.quarantine", value, static_cast<std::size_t>(length), 0, 0) != 0)
            return std::error_code{errno, std::system_category()};
        return {};
#else
        static_cast<void>(path);
        return {};
#endif
    }
}
