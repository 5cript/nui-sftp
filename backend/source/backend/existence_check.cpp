#include <backend/existence_check.hpp>
#include <shared_data/sync/case_fold.hpp>
#include <utility/error_message.hpp>
#include <paths/paths.hpp>

#include <fmt/format.h>
#include <libssh/sftp.h>

#include <filesystem>
#include <optional>
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
    /**
     * @brief Whether @p path exists, nothing but "no such file" counts as absent.
     */
    std::expected<bool, std::string> remotePathExists(
        SecureShell::SftpSession& session,
        Paths::RemotePath const& path,
        std::chrono::milliseconds timeout
    )
    {
        auto future = session.lstat(path);
        if (future.wait_for(timeout) != std::future_status::ready)
            return std::unexpected(fmt::format("'{}': stat timed out", path.value()));
        const auto result = future.get();
        if (!result && result.error().sftpError != SSH_FX_NO_SUCH_FILE)
            return std::unexpected(fmt::format("'{}': {}", path.value(), result.error().toString()));
        return result.has_value();
    }

    /**
     * @brief The names in @p parent, without resolving symlinks. Nothing for a directory that cannot be listed
     *        because of its permissions, like a drop box; an empty set for one that does not exist yet.
     */
    std::expected<std::optional<std::unordered_set<std::string>>, std::string>
    listNames(SecureShell::SftpSession& session, Paths::RemotePath const& parent, std::chrono::milliseconds timeout)
    {
        auto future = session.listDirectory(parent, /*resolveLinks=*/false);
        if (future.wait_for(timeout) != std::future_status::ready)
            return std::unexpected(fmt::format("'{}': listing timed out", parent.value()));
        const auto result = future.get();
        if (!result)
        {
            if (result.error().sftpError == SSH_FX_NO_SUCH_FILE)
                return std::unordered_set<std::string>{};
            if (result.error().sftpError == SSH_FX_PERMISSION_DENIED)
                return std::nullopt;
            return std::unexpected(fmt::format("'{}': {}", parent.value(), result.error().toString()));
        }
        std::unordered_set<std::string> names;
        names.reserve(result->size());
        for (auto const& entry : *result)
            names.insert(entry.name);
        return names;
    }

    /**
     * @brief Whether the queried children of one parent exist, from one listing of it. A name the listing lacks
     *        while it holds a case or normalization variant is looked up, since the server may fold names.
     */
    std::expected<void, std::string> checkByListing(
        SecureShell::SftpSession& session,
        Paths::RemotePath const& parent,
        std::vector<Paths::RemotePath> const& paths,
        std::vector<std::size_t> const& indices,
        std::chrono::milliseconds timeout,
        std::vector<bool>& results
    )
    {
        auto names = listNames(session, parent, timeout);
        if (!names)
            return std::unexpected(names.error());
        if (!*names)
        {
            for (const auto index : indices)
            {
                auto exists = remotePathExists(session, paths[index], timeout);
                if (!exists)
                    return std::unexpected(exists.error());
                results[index] = *exists;
            }
            return {};
        }

        std::optional<std::unordered_set<std::string>> foldedNames{};
        for (const auto index : indices)
        {
            const auto name = std::string{paths[index].filename()->view()};
            if ((*names)->contains(name))
            {
                results[index] = true;
                continue;
            }
            if (!foldedNames)
            {
                foldedNames.emplace();
                for (auto const& listed : **names)
                    foldedNames->insert(SharedData::Sync::caseFoldedName(listed));
            }
            if (!foldedNames->contains(SharedData::Sync::caseFoldedName(name)))
            {
                results[index] = false;
                continue;
            }
            auto exists = remotePathExists(session, paths[index], timeout);
            if (!exists)
                return std::unexpected(exists.error());
            results[index] = *exists;
        }
        return {};
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
    // Parents in the order they first appear, with the indices of their queried children.
    std::vector<std::pair<Paths::RemotePath, std::vector<std::size_t>>> byParent;
    std::unordered_map<std::string, std::size_t> parentIndex;
    for (auto const& path : paths)
    {
        const auto parent = remotePaths[index].parent();
        if (!parent)
            return std::unexpected(fmt::format("'{}' has no parent directory", remotePaths[index].value()));
        const auto [found, inserted] = parentIndex.try_emplace(parent->value(), byParent.size());
        if (inserted)
            byParent.emplace_back(*parent, std::vector<std::size_t>{});
        byParent[found->second].second.push_back(index);
    }

    std::vector<bool> results(remotePaths.size(), false);
    for (auto const& [parent, indices] : byParent)
    {
        if (indices.size() >= remoteListingThreshold)
        {
            if (auto checked = checkByListing(session, parent, remotePaths, indices, timeout, results); !checked)
                return std::unexpected(checked.error());
            continue;
        }
        for (const auto index : indices)
        {
            auto exists = remotePathExists(session, remotePaths[index], timeout);
            if (!exists)
                return std::unexpected(exists.error());
            results[index] = *exists;
        }
    }
    return results;
}
