#include <utility/link_safe_remove.hpp>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

namespace Utility
{
    bool isLinkOrJunction(std::filesystem::path const& path, std::error_code& error)
    {
        error.clear();
#ifdef _WIN32
        const auto attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto lastError = ::GetLastError();
            if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
                error = std::error_code{static_cast<int>(lastError), std::system_category()};
            return false;
        }
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        const auto status = std::filesystem::symlink_status(path, error);
        if (status.type() == std::filesystem::file_type::not_found)
            error.clear();
        return !error && std::filesystem::is_symlink(status);
#endif
    }

    std::optional<std::filesystem::path>
    firstLinkBelow(std::filesystem::path const& root, std::filesystem::path const& path, std::error_code& error)
    {
        error.clear();
        const auto relative = path.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..")
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return std::nullopt;
        }
        auto current = root;
        for (auto const& component : relative)
        {
            if (component == ".")
                continue;
            current /= component;
            const bool isLink = isLinkOrJunction(current, error);
            if (error)
                return std::nullopt;
            if (isLink)
                return current;
        }
        return std::nullopt;
    }

    namespace
    {
        bool removeLink(std::filesystem::path const& path, std::error_code& error)
        {
#ifdef _WIN32
            // A junction or directory symlink is removed like an empty directory, its target stays.
            const auto attributes = ::GetFileAttributesW(path.c_str());
            const bool removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? ::RemoveDirectoryW(path.c_str()) != 0
                                                                               : ::DeleteFileW(path.c_str()) != 0;
            if (!removed)
                error = std::error_code{static_cast<int>(::GetLastError()), std::system_category()};
            return removed;
#else
            return std::filesystem::remove(path, error);
#endif
        }
    }

    std::uintmax_t removeAllNoFollow(std::filesystem::path const& path, std::error_code& error)
    {
        error.clear();
        const bool isLink = isLinkOrJunction(path, error);
        if (error)
            return 0;
        if (isLink)
            return removeLink(path, error) ? 1u : 0u;

        const auto status = std::filesystem::symlink_status(path, error);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            error.clear();
            return 0;
        }
        if (error)
            return 0;

        std::uintmax_t removed = 0;
        if (std::filesystem::is_directory(status))
        {
            std::filesystem::directory_iterator iter{path, error};
            if (error)
                return removed;
            for (; iter != std::filesystem::directory_iterator{}; iter.increment(error))
            {
                if (error)
                    return removed;
                removed += removeAllNoFollow(iter->path(), error);
                if (error)
                    return removed;
            }
            if (error)
                return removed;
        }
        if (std::filesystem::remove(path, error))
            ++removed;
        return removed;
    }
}
