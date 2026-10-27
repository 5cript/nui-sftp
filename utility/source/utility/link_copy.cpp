#include <utility/link_copy.hpp>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <winioctl.h>

#    include <cstddef>
#    include <cstdint>
#    include <cstring>
#    include <memory>
#    include <vector>
#endif

namespace Utility
{
#ifdef _WIN32
    namespace
    {
        struct HandleCloser
        {
            void operator()(HANDLE handle) const noexcept
            {
                if (handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle(handle);
            }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;

        std::error_code lastError()
        {
            return std::error_code{static_cast<int>(::GetLastError()), std::system_category()};
        }

        /**
         * @brief Opens the reparse point @p path itself, never what it points to.
         */
        std::expected<Handle, std::error_code> openReparsePoint(std::filesystem::path const& path, DWORD access)
        {
            Handle handle{::CreateFileW(
                path.c_str(),
                access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                nullptr
            )};
            if (handle.get() == INVALID_HANDLE_VALUE)
                return std::unexpected(lastError());
            return handle;
        }

        /**
         * @brief The reparse data of @p path as Windows stores it, starting with its tag.
         */
        std::expected<std::vector<std::byte>, std::error_code> reparseDataOf(std::filesystem::path const& path)
        {
            return openReparsePoint(path, FILE_READ_ATTRIBUTES)
                .and_then(
                    [](Handle const& handle) -> std::expected<std::vector<std::byte>, std::error_code>
                    {
                        std::vector<std::byte> data(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
                        DWORD size = 0;
                        if (::DeviceIoControl(
                                handle.get(),
                                FSCTL_GET_REPARSE_POINT,
                                nullptr,
                                0,
                                data.data(),
                                static_cast<DWORD>(data.size()),
                                &size,
                                nullptr
                            ) == 0)
                            return std::unexpected(lastError());
                        data.resize(size);
                        return data;
                    }
                );
        }

        std::uint32_t tagOf(std::vector<std::byte> const& data)
        {
            std::uint32_t tag = 0;
            if (data.size() >= sizeof(tag))
                std::memcpy(&tag, data.data(), sizeof(tag));
            return tag;
        }

        /**
         * @brief Creates the directory @p destination and makes it a junction with the reparse data @p data.
         */
        std::expected<void, std::error_code>
        createJunction(std::filesystem::path const& destination, std::vector<std::byte> const& data)
        {
            if (::CreateDirectoryW(destination.c_str(), nullptr) == 0)
                return std::unexpected(lastError());
            auto applied = openReparsePoint(destination, GENERIC_WRITE)
                               .and_then(
                                   [&data](Handle const& handle) -> std::expected<void, std::error_code>
                                   {
                                       DWORD unused = 0;
                                       if (::DeviceIoControl(
                                               handle.get(),
                                               FSCTL_SET_REPARSE_POINT,
                                               const_cast<std::byte*>(data.data()),
                                               static_cast<DWORD>(data.size()),
                                               nullptr,
                                               0,
                                               &unused,
                                               nullptr
                                           ) == 0)
                                           return std::unexpected(lastError());
                                       return {};
                                   }
                               );
            if (!applied)
                ::RemoveDirectoryW(destination.c_str());
            return applied;
        }
    }
#endif

    std::expected<void, std::error_code> copyLink(std::filesystem::path const& source, std::filesystem::path const& destination)
    {
#ifdef _WIN32
        const auto data = reparseDataOf(source);
        if (!data)
            return std::unexpected(data.error());
        if (tagOf(*data) == IO_REPARSE_TAG_MOUNT_POINT)
            return createJunction(destination, *data);
#endif
        std::error_code error{};
        std::filesystem::copy_symlink(source, destination, error);
        if (error)
            return std::unexpected(error);
        return {};
    }
}
