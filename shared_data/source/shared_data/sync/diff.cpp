#include <shared_data/sync/diff.hpp>

#include <map>
#include <utility>

namespace SharedData::Sync
{
    namespace
    {
        std::map<std::string, DirectoryEntry>
        buildEntryMap(std::filesystem::path const& root, std::vector<DirectoryEntry> const& entries)
        {
            std::map<std::string, DirectoryEntry> result;
            for (std::size_t idx = 1; idx < entries.size(); ++idx)
            {
                auto const& entry = entries[idx];
                std::filesystem::path relPath;
                if (entry.fullPath.has_relative_path())
                {
                    relPath = std::filesystem::path{entry.fullPath.generic_string()}.lexically_relative(
                        std::filesystem::path{root.generic_string()}
                    );
                }
                else
                {
                    relPath = entry.path;
                }
                if (!relPath.empty())
                    result.emplace(relPath.generic_string(), entry);
            }
            return result;
        }

        bool hasHiddenSegment(std::string const& relKey)
        {
            std::size_t pos = 0;
            while (pos < relKey.size())
            {
                if (relKey[pos] == '.')
                    return true;
                pos = relKey.find('/', pos);
                if (pos == std::string::npos)
                    break;
                ++pos;
            }
            return false;
        }

        bool isNested(std::string const& relKey)
        {
            return relKey.find('/') != std::string::npos;
        }
    }

    bool entriesDiffer(DirectoryEntry const& localEntry, DirectoryEntry const& remoteEntry)
    {
        if (localEntry.type != remoteEntry.type)
            return true;
        if (localEntry.type == FileType::Symlink)
        {
            if (localEntry.linkTarget && remoteEntry.linkTarget)
                return *localEntry.linkTarget != *remoteEntry.linkTarget;
            return false;
        }
        if (localEntry.size != remoteEntry.size)
            return true;
        if (localEntry.mtime != remoteEntry.mtime)
            return true;
        return false;
    }

    DiffResult computeSyncDiff(
        std::filesystem::path const& localRoot,
        std::filesystem::path const& remoteRoot,
        std::vector<DirectoryEntry> const& localEntries,
        std::vector<DirectoryEntry> const& remoteEntries,
        DiffOptions const& options
    )
    {
        DiffResult result;

        if (localEntries.empty() && remoteEntries.empty())
            return result;

        auto localMap = buildEntryMap(localRoot, localEntries);
        auto remoteMap = buildEntryMap(remoteRoot, remoteEntries);

        if (options.ignoreHidden)
        {
            for (auto mapIter = localMap.begin(); mapIter != localMap.end();)
                mapIter = hasHiddenSegment(mapIter->first) ? localMap.erase(mapIter) : std::next(mapIter);
            for (auto mapIter = remoteMap.begin(); mapIter != remoteMap.end();)
                mapIter = hasHiddenSegment(mapIter->first) ? remoteMap.erase(mapIter) : std::next(mapIter);
        }

        if (!options.recursive)
        {
            for (auto mapIter = localMap.begin(); mapIter != localMap.end();)
                mapIter = isNested(mapIter->first) ? localMap.erase(mapIter) : std::next(mapIter);
            for (auto mapIter = remoteMap.begin(); mapIter != remoteMap.end();)
                mapIter = isNested(mapIter->first) ? remoteMap.erase(mapIter) : std::next(mapIter);
        }

        // ---- Entries that exist locally ----------------------------------
        for (auto const& [relKey, localEntry] : localMap)
        {
            auto remoteIter = remoteMap.find(relKey);
            if (remoteIter == remoteMap.end())
            {
                if (options.actionUpload && options.direction != Direction::Download)
                {
                    result.uploads.push_back(DiffEntry{
                        .relKey = relKey,
                        .action = Action::Upload,
                        .local = localEntry,
                        .remote = std::nullopt,
                    });
                }
                else if (options.actionDelete && options.direction == Direction::Download)
                {
                    // In non-recursive mode the children of this directory weren't scanned,
                    // so deleting it could remove items the user never saw — hide it.
                    const bool skipDir = !options.recursive && localEntry.type == FileType::Directory;
                    if (!skipDir)
                    {
                        result.deletes.push_back(DiffEntry{
                            .relKey = relKey,
                            .action = Action::DeleteLocal,
                            .local = localEntry,
                            .remote = std::nullopt,
                        });
                    }
                }
                continue;
            }

            auto const& remoteEntry = remoteIter->second;

            if (localEntry.type == FileType::Directory)
                continue;
            if (!entriesDiffer(localEntry, remoteEntry))
                continue;

            const bool localNewer = localEntry.mtime >= remoteEntry.mtime;

            if (options.direction == Direction::Upload ||
                (options.direction == Direction::Both && localNewer))
            {
                if (options.actionUpload)
                {
                    result.uploads.push_back(DiffEntry{
                        .relKey = relKey,
                        .action = Action::Upload,
                        .local = localEntry,
                        .remote = remoteEntry,
                    });
                }
            }
            else
            {
                if (options.actionDownload)
                {
                    result.downloads.push_back(DiffEntry{
                        .relKey = relKey,
                        .action = Action::Download,
                        .local = localEntry,
                        .remote = remoteEntry,
                    });
                }
            }
        }

        // ---- Entries that exist only remotely ----------------------------
        for (auto const& [relKey, remoteEntry] : remoteMap)
        {
            if (localMap.count(relKey))
                continue;

            if (options.actionDownload && options.direction != Direction::Upload)
            {
                result.downloads.push_back(DiffEntry{
                    .relKey = relKey,
                    .action = Action::Download,
                    .local = std::nullopt,
                    .remote = remoteEntry,
                });
            }
            else if (options.actionDelete && options.direction == Direction::Upload)
            {
                const bool skipDir = !options.recursive && remoteEntry.type == FileType::Directory;
                if (!skipDir)
                {
                    result.deletes.push_back(DiffEntry{
                        .relKey = relKey,
                        .action = Action::DeleteRemote,
                        .local = std::nullopt,
                        .remote = remoteEntry,
                    });
                }
            }
        }

        return result;
    }
}
