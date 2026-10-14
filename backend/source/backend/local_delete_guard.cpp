#include <backend/local_delete_guard.hpp>
#include <utility/error_message.hpp>
#include <log/log.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <shlobj.h>
#else
#    include <pwd.h>
#    include <unistd.h>
#endif

namespace
{
    bool foldsCase([[maybe_unused]] Paths::LocalPath const& path)
    {
#ifdef __APPLE__
        return true;
#else
        return path.platform() == Paths::LocalPlatform::Windows;
#endif
    }

    bool containsAsText(Paths::LocalPath const& target, Paths::LocalPath const& home)
    {
        return home.isWithin(target) || (foldsCase(target) && home.isWithinIgnoringCase(target));
    }

    /**
     * @brief True when @p target is an existing directory that is @p home or one of its ancestors on disk.
     */
    bool isHomeOrAncestorOnDisk(Paths::LocalPath const& target, Paths::LocalPath const& home)
    {
        if (target.platform() != Paths::compiledLocalPlatform() || home.platform() != target.platform())
            return false;
        const auto native = target.native();
        std::error_code error{};
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(native, error)))
            return false;
        for (std::optional<Paths::LocalPath> ancestor = home; ancestor; ancestor = ancestor->parent())
        {
            if (std::filesystem::equivalent(ancestor->native(), native, error) && !error)
                return true;
        }
        return false;
    }

    void addHome(std::vector<Paths::LocalPath>& homes, std::filesystem::path const& native)
    {
        if (native.empty())
            return;
        try
        {
            auto home = Paths::LocalPath::make(native);
            if (!home)
            {
                Log::warn("Ignoring the home directory candidate {}", home.error().toString());
                return;
            }
            if (std::ranges::find(homes, *home) == homes.end())
                homes.push_back(*std::move(home));
        }
        catch (std::exception const& exception)
        {
            Log::warn(
                "Ignoring a home directory candidate that is not representable: {}", Utility::errorMessage(exception)
            );
        }
    }

#ifdef _WIN32
    struct CoTaskMemoryDeleter
    {
        void operator()(wchar_t* memory) const noexcept
        {
            CoTaskMemFree(memory);
        }
    };

    void addEnvironmentHome(std::vector<Paths::LocalPath>& homes, wchar_t const* name)
    {
        if (auto const* value = _wgetenv(name); value != nullptr && *value != L'\0')
            addHome(homes, std::filesystem::path{value});
    }
#endif
}

std::expected<void, std::string>
refuseProtectedLocalDelete(Paths::LocalPath const& target, std::vector<Paths::LocalPath> const& homes)
{
    if (target.empty())
        return std::unexpected(std::string{"no path"});
    if (target.isRoot())
        return std::unexpected(fmt::format("'{}' is a root directory", target.toUtf8()));
    const auto protectedHome = std::ranges::find_if(
        homes,
        [&target](Paths::LocalPath const& home)
        {
            return containsAsText(target, home) || isHomeOrAncestorOnDisk(target, home);
        }
    );
    if (protectedHome != homes.end())
        return std::unexpected(
            fmt::format("'{}' is or contains the home directory '{}'", target.toUtf8(), protectedHome->toUtf8())
        );
    return {};
}

std::expected<Paths::LocalPath, std::string>
localDeleteTarget(std::string const& utf8, std::vector<Paths::LocalPath> const& homes)
{
    auto target = Paths::LocalPath::fromUtf8(utf8);
    if (!target)
        return std::unexpected(target.error().toString());

    // LocalPath resolves ".." lexically, which goes elsewhere when a directory on the way is a symlink.
    std::string generic{utf8};
    if (target->platform() == Paths::LocalPlatform::Windows)
        std::ranges::replace(generic, '\\', '/');
    const auto isParentReference = [](std::string_view name)
    {
        return name == "..";
    };
    if (std::ranges::any_of(Paths::Detail::splitAtSlashes(generic), isParentReference))
        return std::unexpected(fmt::format("'{}' contains '..'", utf8));

    if (auto allowed = refuseProtectedLocalDelete(*target, homes); !allowed)
        return std::unexpected(std::move(allowed).error());
    return *std::move(target);
}

std::vector<Paths::LocalPath> localHomeDirectories()
{
    std::vector<Paths::LocalPath> homes;
#ifdef _WIN32
    PWSTR rawProfile = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &rawProfile);
    const std::unique_ptr<wchar_t, CoTaskMemoryDeleter> profile{rawProfile};
    if (SUCCEEDED(result) && profile != nullptr)
        addHome(homes, std::filesystem::path{profile.get()});
    addEnvironmentHome(homes, L"USERPROFILE");
    addEnvironmentHome(homes, L"HOME");
#else
    if (auto const* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        addHome(homes, std::filesystem::path{home});

    const auto suggestedSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggestedSize > 0 ? static_cast<std::size_t>(suggestedSize) : 16384u);
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr)
        addHome(homes, std::filesystem::path{found->pw_dir});
#endif
    if (homes.empty())
        Log::warn("The home directory is unknown, local deletes only refuse root directories.");
    return homes;
}
