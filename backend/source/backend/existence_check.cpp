#include <backend/existence_check.hpp>
#include <utility/error_message.hpp>
#include <paths/paths.hpp>

#include <fmt/format.h>
#include <libssh/sftp.h>

#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

std::expected<std::vector<bool>, std::string> localPathsExist(std::vector<std::string> const& paths)
{
    std::vector<bool> results;
    results.reserve(paths.size());
    for (auto const& pathUtf8 : paths)
    {
        const auto path = Paths::LocalPath::fromUtf8(pathUtf8);
        if (!path)
            return std::unexpected(path.error().toString());
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path->native(), ec);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            results.push_back(false);
            continue;
        }
        if (ec)
            return std::unexpected(fmt::format("'{}': {}", path.toUtf8(), Utility::errorMessage(ec)));
        results.push_back(true);
    }
    return results;
}

namespace
{
    // Below this many paths a stat per path is cheaper than reading a potentially huge directory in full.
    constexpr std::size_t listingThreshold = 10;

    std::expected<std::vector<bool>, std::string> remotePathsExistByStat(
        SecureShell::SftpSession& session,
        std::vector<Paths::RemotePath> const& paths,
        std::chrono::milliseconds timeout
    )
    {
        std::vector<bool> results;
        results.reserve(paths.size());
        for (auto const& path : paths)
        {
            auto future = session.lstat(path);
            if (future.wait_for(timeout) != std::future_status::ready)
                return std::unexpected(fmt::format("'{}': stat timed out", path.value()));
            const auto result = future.get();
            if (!result && result.error().sftpError != SSH_FX_NO_SUCH_FILE)
                return std::unexpected(fmt::format("'{}': {}", path.value(), result.error().toString()));
            results.push_back(result.has_value());
        }
        return results;
    }

    /**
     * @brief One listing per parent directory instead of a stat per path. A parent that does not exist yet holds
     *        nothing.
     */
    std::expected<std::vector<bool>, std::string> remotePathsExistByListing(
        SecureShell::SftpSession& session,
        std::vector<Paths::RemotePath> const& paths,
        std::chrono::milliseconds timeout
    )
    {
        const auto listParent =
            [&session, timeout](Paths::RemotePath const& parent) -> std::expected<std::unordered_set<std::string>, std::string>
        {
            auto future = session.listDirectory(parent);
            if (future.wait_for(timeout) != std::future_status::ready)
                return std::unexpected(fmt::format("'{}': listing timed out", parent.value()));
            const auto result = future.get();
            if (!result)
            {
                if (result.error().sftpError == SSH_FX_NO_SUCH_FILE)
                    return std::unordered_set<std::string>{};
                return std::unexpected(fmt::format("'{}': {}", parent.value(), result.error().toString()));
            }
            std::unordered_set<std::string> names;
            names.reserve(result->size());
            for (auto const& entry : *result)
                names.insert(entry.name);
            return names;
        };

        std::unordered_map<std::string, std::unordered_set<std::string>> listingByParent;
        std::vector<bool> results;
        results.reserve(paths.size());
        for (auto const& path : paths)
        {
            const auto parent = path.parent();
            if (!parent)
                return std::unexpected(fmt::format("'{}' has no parent directory", path.value()));
            auto iter = listingByParent.find(parent->value());
            if (iter == listingByParent.end())
            {
                auto listing = listParent(*parent);
                if (!listing)
                    return std::unexpected(listing.error());
                iter = listingByParent.emplace(parent->value(), *std::move(listing)).first;
            }
            results.push_back(iter->second.contains(std::string{path.filename()->view()}));
        }
        return results;
    }
}

std::expected<std::vector<bool>, std::string> remotePathsExist(
    SecureShell::SftpSession& session,
    std::vector<std::string> const& paths,
    std::chrono::milliseconds timeout
)
{
    std::vector<Paths::RemotePath> remotePaths;
    remotePaths.reserve(paths.size());
    for (auto const& path : paths)
    {
        auto remotePath = Paths::RemotePath::make(path);
        if (!remotePath)
            return std::unexpected(remotePath.error().toString());
        remotePaths.push_back(*std::move(remotePath));
    }
    if (remotePaths.size() < listingThreshold)
        return remotePathsExistByStat(session, remotePaths, timeout);
    return remotePathsExistByListing(session, remotePaths, timeout);
}
