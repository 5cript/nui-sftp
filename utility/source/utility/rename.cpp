#include <utility/rename.hpp>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <cerrno>
#    include <cstdio>
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace Utility
{
    namespace
    {
        std::unexpected<std::error_code> lastErrno()
        {
            return std::unexpected(std::error_code{errno, std::generic_category()});
        }
    }

    std::expected<void, std::error_code> renameNoReplace(std::filesystem::path const& from, std::filesystem::path const& to)
    {
#if defined(_WIN32)
        if (::MoveFileExW(from.c_str(), to.c_str(), 0) == 0)
        {
            const auto error = ::GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
                return std::unexpected(std::make_error_code(std::errc::file_exists));
            return std::unexpected(std::error_code{static_cast<int>(error), std::system_category()});
        }
        return {};
#elif defined(__APPLE__)
        if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) != 0)
            return lastErrno();
        return {};
#else
        if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
            return {};
        if (errno != EINVAL && errno != ENOSYS)
            return lastErrno();

        // The filesystem cannot rename without replacing. A hard link fails on an existing target as well.
        std::error_code statusError{};
        if (std::filesystem::is_regular_file(std::filesystem::symlink_status(from, statusError)))
        {
            if (::link(from.c_str(), to.c_str()) != 0)
                return lastErrno();
            if (::unlink(from.c_str()) != 0)
                return lastErrno();
            return {};
        }
        if (std::filesystem::exists(std::filesystem::symlink_status(to, statusError)))
            return std::unexpected(std::make_error_code(std::errc::file_exists));
        if (::rename(from.c_str(), to.c_str()) != 0)
            return lastErrno();
        return {};
#endif
    }
}
