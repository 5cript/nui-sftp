#include <backend/local_removal.hpp>
#include <backend/local_delete_guard.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <log/log.hpp>

#include <fmt/format.h>

#include <filesystem>
#include <system_error>

std::expected<LocalRemoval, LocalRemovalRefusal>
removeLocalItems(std::span<std::string const> pathsUtf8, std::vector<Paths::LocalPath> const& homes)
{
    std::vector<Paths::LocalPath> targets;
    targets.reserve(pathsUtf8.size());
    for (auto const& pathUtf8 : pathsUtf8)
    {
        auto target = localDeleteTarget(pathUtf8, homes);
        if (!target)
            return std::unexpected(LocalRemovalRefusal{.path = pathUtf8, .reason = std::move(target).error()});
        targets.push_back(*std::move(target));
    }

    LocalRemoval removal{};
    const auto addFailure = [&removal](std::filesystem::path const& path, std::error_code const& error)
    {
        const auto pathUtf8 = Paths::nativeToUtf8(path);
        const auto message = Utility::errorMessage(error);
        Log::error("Failed to remove local item '{}': {}", pathUtf8, message);
        removal.failures.push_back(fmt::format("{}: {}", pathUtf8, message));
    };
    for (auto const& target : targets)
    {
        std::error_code error{};
        if (Utility::removeNoFollow(target.native(), error))
            ++removal.removed;
        if (error)
            addFailure(target.native(), error);
    }
    return removal;
}
