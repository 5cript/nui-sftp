#include <utility/checked_symlink_status.hpp>
#include <utility/extended_length_path.hpp>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace Utility
{
    namespace
    {
#ifdef _WIN32
        std::error_code systemError(DWORD code)
        {
            return std::error_code{static_cast<int>(code), std::system_category()};
        }

        using NtStatusReader = LONG(WINAPI*)();

        /**
         * @brief ntdll's RtlGetLastNtStatus, nothing when it is missing. Looked up before the call whose status it
         *        reads.
         */
        NtStatusReader ntStatusReader()
        {
            static const auto reader = reinterpret_cast<NtStatusReader>(
                reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetLastNtStatus"))
            );
            return reader;
        }

        /**
         * @brief What Windows itself says about @p path, which @p readStatus did not find (@p failure unset) or could
         *        not check (@p failure). It tells apart what std::filesystem reports alike: a path past MAX_PATH from a
         *        missing one, and a file being deleted from one that denies access. A shorter path Windows finds was
         *        written anew meanwhile (git deletes and writes files) and gets one more look when @p lookAgain.
         */
        std::expected<std::optional<std::filesystem::file_status>, std::error_code> askWindows(
            std::filesystem::path const& path,
            std::error_code failure,
            Detail::SymlinkStatusReader const& readStatus,
            bool lookAgain
        );

        std::expected<std::optional<std::filesystem::file_status>, std::error_code>
        statusOf(std::filesystem::path const& path, Detail::SymlinkStatusReader const& readStatus, bool lookAgain)
        {
            std::error_code statusError{};
            const auto status = readStatus(path, statusError);
            const bool notFound = status.type() == std::filesystem::file_type::not_found;
            if (!notFound && !statusError)
                return status;
            return askWindows(path, notFound ? std::error_code{} : statusError, readStatus, lookAgain);
        }

        std::expected<std::optional<std::filesystem::file_status>, std::error_code> askWindows(
            std::filesystem::path const& path,
            std::error_code failure,
            Detail::SymlinkStatusReader const& readStatus,
            bool lookAgain
        )
        {
            const auto readNtStatus = ntStatusReader();
            const auto attributes = ::GetFileAttributesW(extendedLengthPath(path).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                if (failure)
                    return std::unexpected(failure);
                if (path.native().size() >= MAX_PATH)
                    return std::unexpected(systemError(ERROR_FILENAME_EXCED_RANGE));
                if (lookAgain)
                    return statusOf(path, readStatus, /*lookAgain=*/false);
                return std::unexpected(systemError(ERROR_INVALID_NAME));
            }
            const auto lastError = ::GetLastError();
            if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
                return std::nullopt;
            // Deleted without POSIX semantics (libc++'s remove), it and what is below it stay in the way as
            // STATUS_DELETE_PENDING, which reads as access denied, until the last handle to it closes.
            constexpr auto deletePending = static_cast<LONG>(0xC0000056u);
            if (lastError == ERROR_ACCESS_DENIED && readNtStatus && readNtStatus() == deletePending)
                return std::nullopt;
            return std::unexpected(systemError(lastError));
        }
#endif
    }

    namespace Detail
    {
        std::expected<std::optional<std::filesystem::file_status>, std::error_code>
        checkedSymlinkStatus(std::filesystem::path const& path, SymlinkStatusReader const& readStatus)
        {
#ifdef _WIN32
            return statusOf(path, readStatus, /*lookAgain=*/true);
#else
            std::error_code statusError{};
            const auto status = readStatus(path, statusError);
            const bool notFound = status.type() == std::filesystem::file_type::not_found;
            if (!notFound && !statusError)
                return status;
            if (notFound)
                return std::nullopt;
            return std::unexpected(statusError);
#endif
        }
    }

    std::expected<std::optional<std::filesystem::file_status>, std::error_code>
    checkedSymlinkStatus(std::filesystem::path const& path)
    {
        return Detail::checkedSymlinkStatus(
            path,
            [](std::filesystem::path const& looked, std::error_code& error)
            {
                return std::filesystem::symlink_status(looked, error);
            }
        );
    }
}
