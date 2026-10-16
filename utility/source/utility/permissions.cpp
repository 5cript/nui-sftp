#include <utility/permissions.hpp>

#include <fstream>
#include <string>

#ifndef _WIN32
#    include <cerrno>
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
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

#ifndef _WIN32
        class UniqueDescriptor
        {
          public:
            explicit UniqueDescriptor(int descriptor)
                : descriptor_{descriptor}
            {}
            ~UniqueDescriptor()
            {
                if (descriptor_ >= 0)
                    ::close(descriptor_);
            }
            UniqueDescriptor(UniqueDescriptor const&) = delete;
            UniqueDescriptor& operator=(UniqueDescriptor const&) = delete;
            UniqueDescriptor(UniqueDescriptor&&) = delete;
            UniqueDescriptor& operator=(UniqueDescriptor&&) = delete;

            int get() const
            {
                return descriptor_;
            }

          private:
            int descriptor_;
        };
#endif
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

    std::filesystem::perms newFilePermissions()
    {
        constexpr auto readWrite = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::group_write |
            std::filesystem::perms::others_read | std::filesystem::perms::others_write;
        return readWrite & ~processUmask();
    }

    std::expected<void, std::error_code> restrictToOwner(std::filesystem::path const& path)
    {
#ifdef _WIN32
        static_cast<void>(path);
        return {};
#else
        const UniqueDescriptor descriptor{
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        };
        if (descriptor.get() < 0)
            return std::unexpected(std::error_code{errno, std::generic_category()});
        if (::fchmod(descriptor.get(), S_IRUSR | S_IWUSR) != 0)
            return std::unexpected(std::error_code{errno, std::generic_category()});
        return {};
#endif
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
