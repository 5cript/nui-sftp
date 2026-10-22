#include <backend/file_tracking/local_watch_manager.hpp>
#include <backend/file_tracking/change_filter.hpp>
#include <utility/error_message.hpp>
#include <utility/localized_message.hpp>
#include <utility/algorithm/case_convert.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <ranges>
#include <system_error>
#include <utility>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief @p path in lower case where the file systems fold case (Windows, macOS), unchanged elsewhere.
         */
        std::filesystem::path foldedSpelling(std::filesystem::path const& path)
        {
#if defined(_WIN32) || defined(__APPLE__)
            return Paths::nativeFromUtf8(Utility::Algorithm::toLowerCase(Paths::nativeToUtf8(path)));
#else
            return path;
#endif
        }
    }

    namespace LocalWatchManagerDetail
    {
        PlacedPath::PlacedPath(std::filesystem::path path)
            : PlacedPath{path, DiskPlace::of(path)}
        {}

        PlacedPath::PlacedPath(std::filesystem::path path, DiskPlace place)
            : path{std::move(path)}
            , place{std::move(place)}
        {}

        bool overlaps(PlacedPath const& first, PlacedPath const& second)
        {
            const auto firstSpelling = foldedSpelling(first.path);
            const auto secondSpelling = foldedSpelling(second.path);
            if (isAtOrBelow(firstSpelling, secondSpelling) || isAtOrBelow(secondSpelling, firstSpelling))
                return true;
            // A file system that folds case where the platform does not (vfat, case-folding ext4) or two mounts of
            // one directory show only on disk.
            return first.place.isAtOrBelow(second.place) || second.place.isAtOrBelow(first.place);
        }

        std::expected<void, std::string> refuseOverlap(
            PlacedPath const& item,
            std::vector<PlacedPath> const& watchedItems,
            PlacedPath const& temporaryDownloadsDirectory
        )
        {
            if (overlaps(item, temporaryDownloadsDirectory))
                return std::unexpected{Utility::localizedMessage(
                    "backend.fileTracking.watchOverlapsDownloads",
                    Paths::nativeToUtf8(item.path),
                    Paths::nativeToUtf8(temporaryDownloadsDirectory.path)
                )};
            const auto overlapping = std::ranges::find_if(
                watchedItems,
                [&item](PlacedPath const& watched)
                {
                    return overlaps(item, watched);
                }
            );
            if (overlapping == watchedItems.end())
                return {};
            return std::unexpected{Utility::localizedMessage(
                "backend.fileTracking.watchOverlaps",
                Paths::nativeToUtf8(item.path),
                Paths::nativeToUtf8(overlapping->path)
            )};
        }
    }

    LocalWatchManager::LocalWatchManager(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        std::filesystem::path temporaryDownloadsDirectory
    )
        : strand_{std::move(strand)}
        , temporaryDownloadsDirectory_{std::move(temporaryDownloadsDirectory)}
        , watches_{}
    {}

    std::expected<std::filesystem::path, std::string>
    LocalWatchManager::checkWatchable(std::filesystem::path const& item) const
    {
        std::error_code canonicalError;
        auto canonical = std::filesystem::canonical(item, canonicalError);
        if (canonicalError)
            return std::unexpected{Utility::localizedMessage(
                "backend.fileTracking.watchUnreadable", Paths::nativeToUtf8(item), Utility::errorMessage(canonicalError)
            )};
        if (!canonical.has_relative_path())
            return std::unexpected{Utility::localizedMessage("backend.fileTracking.watchRoot", Paths::nativeToUtf8(item))};
        return refuseOverlapWithWatches(canonical)
            .transform(
                [&canonical]()
                {
                    return std::move(canonical);
                }
            );
    }

    std::expected<void, std::string> LocalWatchManager::refuseOverlapWithWatches(
        std::filesystem::path const& item,
        std::optional<Ids::WatchId> const& ignoredWatchId
    ) const
    {
        const auto watchedItems = watches_ |
            std::views::filter(
                [&ignoredWatchId](auto const& watched)
                {
                    return watched.first != ignoredWatchId;
                }
            ) |
            std::views::values |
            std::views::transform(
                [](LocalItemWatch const& watched)
                {
                    // Taken when the watch started or followed its item, nothing else is looked at on disk.
                    return LocalWatchManagerDetail::PlacedPath{watched.item(), watched.place()};
                }
            ) |
            std::ranges::to<std::vector>();
        std::error_code temporaryError;
        return LocalWatchManagerDetail::refuseOverlap(
            item, watchedItems, std::filesystem::weakly_canonical(temporaryDownloadsDirectory_, temporaryError)
        );
    }

    std::expected<LocalItemWatch const*, std::string> LocalWatchManager::watch(
        Ids::WatchId const& watchId,
        std::filesystem::path const& item,
        std::string const& downloadTempSuffix,
        ChangeSink sink
    )
    {
        return checkWatchable(item)
            .and_then(
                [this, &watchId, &downloadTempSuffix, &sink](std::filesystem::path const& canonical)
                {
                    // A rename beside itself overlaps the watch's own item only in case, where that is folded.
                    return LocalItemWatch::open(
                        watchId,
                        canonical,
                        strand_,
                        std::move(sink),
                        downloadTempSuffix,
                        [this](Ids::WatchId const& watchId, std::filesystem::path const& renamed)
                        {
                            return refuseOverlapWithWatches(renamed, watchId).has_value();
                        }
                    );
                }
            )
            .transform(
                [this](LocalItemWatch watched) -> LocalItemWatch const*
                {
                    Log::info(
                        "FileTracking: watching local '{}' as {}",
                        Paths::nativeToUtf8(watched.item()),
                        watched.watchId().value()
                    );
                    auto id = watched.watchId();
                    const auto [entry, inserted] = watches_.emplace(std::move(id), std::move(watched));
                    return &entry->second;
                }
            );
    }

    void LocalWatchManager::unwatch(Ids::WatchId const& watchId)
    {
        if (watches_.erase(watchId) > 0)
            Log::info("FileTracking: stopped watching local {}", watchId.value());
    }

    LocalItemWatch const* LocalWatchManager::find(Ids::WatchId const& watchId) const
    {
        const auto found = watches_.find(watchId);
        return found == watches_.end() ? nullptr : &found->second;
    }
}
