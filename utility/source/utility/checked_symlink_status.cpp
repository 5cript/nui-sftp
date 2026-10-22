#include <utility/checked_symlink_status.hpp>

#include <string>

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

        /**
         * @brief @p path as an extended-length path (\\?\C:\x or \\?\UNC\server\share\x), which MAX_PATH does not
         *        limit. Nothing for a relative path and for one in the device namespace already.
         */
        std::optional<std::wstring> extendedLengthPathOf(std::filesystem::path const& path)
        {
            // The extended-length form skips the normalization of separators, "." and "..".
            const std::wstring native = path.lexically_normal().make_preferred().native();
            if (native.starts_with(LR"(\\?\)") || native.starts_with(LR"(\\.\)"))
                return std::nullopt;
            if (native.starts_with(LR"(\\)"))
                return LR"(\\?\UNC\)" + native.substr(2);
            if (native.size() >= 3 && native[1] == L':' && native[2] == L'\\')
                return LR"(\\?\)" + native;
            return std::nullopt;
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
         * @brief What Windows itself says about @p path, which std::filesystem did not find (@p failure unset) or
         *        could not check (@p failure). It tells apart what std::filesystem reports alike: a path past MAX_PATH
         *        from a missing one, and a file being deleted from one that denies access.
         */
        std::expected<std::optional<std::filesystem::file_status>, std::error_code>
        askWindows(std::filesystem::path const& path, std::error_code failure)
        {
            const auto readNtStatus = ntStatusReader();
            const auto extended = extendedLengthPathOf(path);
            const auto attributes = ::GetFileAttributesW(extended ? extended->c_str() : path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                if (failure)
                    return std::unexpected(failure);
                return std::unexpected(
                    systemError(path.native().size() >= MAX_PATH ? ERROR_FILENAME_EXCED_RANGE : ERROR_INVALID_NAME)
                );
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

    std::expected<std::optional<std::filesystem::file_status>, std::error_code>
    checkedSymlinkStatus(std::filesystem::path const& path)
    {
        std::error_code statusError{};
        const auto status = std::filesystem::symlink_status(path, statusError);
        const bool notFound = status.type() == std::filesystem::file_type::not_found;
        if (!notFound && !statusError)
            return status;
#ifdef _WIN32
        return askWindows(path, notFound ? std::error_code{} : statusError);
#else
        if (notFound)
            return std::nullopt;
        return std::unexpected(statusError);
#endif
    }
}
