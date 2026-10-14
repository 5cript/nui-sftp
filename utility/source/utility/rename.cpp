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
#ifndef _WIN32
        std::error_code lastErrno()
        {
            return std::error_code{errno, std::generic_category()};
        }
#endif
    }

    namespace Detail
    {
        bool isNoReplaceUnsupported(std::error_code error)
        {
            return error == std::errc::invalid_argument || error == std::errc::function_not_supported ||
                error == std::errc::operation_not_supported || error == std::errc::not_supported ||
                error == std::errc::operation_not_permitted;
        }

        std::expected<void, std::error_code>
        renameByLink(std::filesystem::path const& from, std::filesystem::path const& to)
        {
            std::error_code error{};
            std::filesystem::create_hard_link(from, to, error);
            if (error)
                return std::unexpected(error);
            std::filesystem::remove(from, error);
            if (error)
            {
                std::error_code undoError{};
                std::filesystem::remove(to, undoError);
                return std::unexpected(error);
            }
            return {};
        }

        std::expected<void, std::error_code>
        renameUnlessExists(std::filesystem::path const& from, std::filesystem::path const& to)
        {
            std::error_code error{};
            const auto status = std::filesystem::symlink_status(to, error);
            if (status.type() == std::filesystem::file_type::none)
                return std::unexpected(error);
            if (std::filesystem::exists(status))
                return std::unexpected(std::make_error_code(std::errc::file_exists));
            error.clear();
            std::filesystem::rename(from, to, error);
            if (error)
                return std::unexpected(error);
            return {};
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
        if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0)
            return {};
        if (const auto error = lastErrno(); !Detail::isNoReplaceUnsupported(error))
            return std::unexpected(error);
        return Detail::renameUnlessExists(from, to);
#else
        if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
            return {};
        if (const auto error = lastErrno(); !Detail::isNoReplaceUnsupported(error))
            return std::unexpected(error);

        std::error_code statusError{};
        if (std::filesystem::is_regular_file(std::filesystem::symlink_status(from, statusError)))
        {
            const auto linked = Detail::renameByLink(from, to);
            if (linked || !Detail::isNoReplaceUnsupported(linked.error()))
                return linked;
        }
        return Detail::renameUnlessExists(from, to);
#endif
    }

    std::expected<void, std::error_code>
    moveIntoPlace(std::filesystem::path const& from, std::filesystem::path const& to, bool replace)
    {
        if (!replace)
            return renameNoReplace(from, to);
        std::error_code error{};
        std::filesystem::rename(from, to, error);
        if (error)
            return std::unexpected(error);
        return {};
    }
}
