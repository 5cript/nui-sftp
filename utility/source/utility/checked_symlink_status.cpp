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

        /**
         * @brief Whether @p path, which std::filesystem did not find, does not exist: the error Windows itself gives
         *        for its extended-length form tells.
         */
        std::expected<std::optional<std::filesystem::file_status>, std::error_code>
        confirmedMissing(std::filesystem::path const& path)
        {
            const auto extended = extendedLengthPathOf(path);
            const auto attributes = ::GetFileAttributesW(extended ? extended->c_str() : path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
                return std::unexpected(
                    systemError(path.native().size() >= MAX_PATH ? ERROR_FILENAME_EXCED_RANGE : ERROR_INVALID_NAME)
                );
            const auto lastError = ::GetLastError();
            if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
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
        if (status.type() == std::filesystem::file_type::not_found)
        {
#ifdef _WIN32
            return confirmedMissing(path);
#else
            return std::nullopt;
#endif
        }
        if (statusError)
            return std::unexpected(statusError);
        return status;
    }
}
