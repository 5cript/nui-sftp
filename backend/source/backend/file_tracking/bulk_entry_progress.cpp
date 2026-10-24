#include <backend/file_tracking/bulk_entry_progress.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace FileTracking
{
    namespace
    {
        bool sameFile(SharedData::WatchFileProgress const& left, SharedData::WatchFileProgress const& right)
        {
            return left.path == right.path && left.bytes == right.bytes && left.totalBytes == right.totalBytes;
        }

        UploadProgress onItsWay(Paths::LocalPath const& source, std::uint64_t bytes, std::uint64_t totalBytes)
        {
            const auto sent = std::min(bytes, totalBytes);
            return {
                .bytesSent = sent,
                .bytesTotal = totalBytes,
                .inFlight = {{.path = source, .bytes = sent, .totalBytes = totalBytes}},
                .filesDone = 0u,
                .fileCount = 1u,
            };
        }

        UploadProgress done(std::uint64_t totalBytes)
        {
            return {
                .bytesSent = totalBytes,
                .bytesTotal = totalBytes,
                .filesDone = 1u,
                .fileCount = 1u,
                .transferred = true,
            };
        }

        UploadProgress leftOut(std::uint64_t sent, std::uint64_t totalBytes)
        {
            return {.bytesSent = sent, .bytesTotal = totalBytes, .filesDone = 0u, .fileCount = 1u};
        }

        /**
         * @brief Remembers @p now as @p told and adds it to @p changed unless it was told already.
         */
        void tellChanged(
            std::optional<UploadProgress>& told,
            std::size_t index,
            UploadProgress now,
            std::vector<BulkEntryProgress::EntryProgress>& changed
        )
        {
            if (told && sameProgress(*told, now))
                return;
            told = now;
            changed.push_back({.index = index, .progress = std::move(now)});
        }
    }

    BulkEntryProgress::BulkEntryProgress(std::vector<Entry> entries, std::optional<Directory> directory)
        : entries_{std::move(entries)}
        , directory_{std::move(directory)}
    {}

    BulkEntryProgress BulkEntryProgress::ofFiles(std::vector<File> files)
    {
        std::vector<Entry> entries;
        entries.reserve(files.size());
        for (auto& file : files)
            entries.push_back({.file = std::move(file)});
        return BulkEntryProgress{std::move(entries), std::nullopt};
    }

    BulkEntryProgress BulkEntryProgress::ofDirectory(Paths::LocalPath source, Paths::RemotePath destination)
    {
        return BulkEntryProgress{{}, Directory{.source = std::move(source), .destination = std::move(destination)}};
    }

    std::vector<BulkEntryProgress::EntryProgress> BulkEntryProgress::update(SharedData::BulkProgress const& progress)
    {
        if (directory_)
            return updateDirectory(progress);
        return updateFiles(progress);
    }

    std::vector<BulkEntryProgress::EntryProgress>
    BulkEntryProgress::updateFiles(SharedData::BulkProgress const& progress)
    {
        const std::set<std::uint64_t> leftOutIndices(
            progress.failedEntryIndices.begin(), progress.failedEntryIndices.end()
        );
        std::map<std::uint64_t, SharedData::BulkFileProgress const*> onTheirWay;
        for (auto const& file : progress.inFlight)
            onTheirWay.emplace(file.entryIndex, &file);
        // Every entry ended, those that were never seen on their way too.
        const bool through = progress.fileCount > 0u && progress.fileCurrentIndex >= progress.fileCount;

        std::vector<EntryProgress> changed;
        for (std::size_t index = 0; index < entries_.size(); ++index)
        {
            auto& entry = entries_[index];
            if (const auto found = onTheirWay.find(index); found != onTheirWay.end())
            {
                entry.started = true;
                entry.file.sizeBytes = found->second->totalBytes;
                tellChanged(
                    entry.told, index, onItsWay(entry.file.source, found->second->bytes, entry.file.sizeBytes), changed
                );
            }
            else if (leftOutIndices.contains(index))
                tellChanged(
                    entry.told, index, leftOut(entry.told ? entry.told->bytesSent : 0u, entry.file.sizeBytes), changed
                );
            else if (entry.started || through)
                tellChanged(entry.told, index, done(entry.file.sizeBytes), changed);
        }
        return changed;
    }

    std::vector<BulkEntryProgress::EntryProgress>
    BulkEntryProgress::updateDirectory(SharedData::BulkProgress const& progress)
    {
        auto& directory = *directory_;
        UploadProgress now{
            .bytesSent = progress.bytesCurrent,
            .bytesTotal = progress.bytesTotal,
            .filesDone = progress.fileCurrentIndex,
            .fileCount = progress.fileCount,
        };
        for (auto const& file : progress.inFlight)
        {
            const auto local = Paths::RemotePath::make(file.file)
                                   .and_then(
                                       [&directory](Paths::RemotePath const& remote)
                                       {
                                           return remote.relativeTo(directory.destination);
                                       }
                                   )
                                   .and_then(
                                       [&directory](Paths::RelativePath const& relative)
                                       {
                                           return directory.source.join(relative);
                                       }
                                   );
            if (local)
                now.inFlight.push_back({.path = *local, .bytes = file.bytes, .totalBytes = file.totalBytes});
        }
        now.transferred =
            progress.fileCount > 0u && progress.fileCurrentIndex >= progress.fileCount && progress.inFlight.empty();

        std::vector<EntryProgress> changed;
        tellChanged(directory.told, 0u, std::move(now), changed);
        return changed;
    }

    UploadProgress uploadProgressOf(SharedData::TransferProgress const& progress, Paths::LocalPath const& source)
    {
        const auto total = progress.max > progress.min ? progress.max - progress.min : 0u;
        const auto position = progress.current > progress.min ? progress.current - progress.min : 0u;
        const auto sent = std::min(position, total);
        const bool transferred = progress.current >= progress.max;
        UploadProgress result{
            .bytesSent = sent,
            .bytesTotal = total,
            .filesDone = transferred ? 1u : 0u,
            .fileCount = 1u,
            .transferred = transferred,
        };
        if (!transferred)
            result.inFlight.push_back({.path = source, .bytes = sent, .totalBytes = total});
        return result;
    }

    bool sameProgress(UploadProgress const& left, UploadProgress const& right)
    {
        return left.bytesSent == right.bytesSent && left.bytesTotal == right.bytesTotal &&
            left.filesDone == right.filesDone && left.fileCount == right.fileCount &&
            left.transferred == right.transferred && std::ranges::equal(left.inFlight, right.inFlight, sameFile);
    }
}
