#include <backend/file_tracking/watch_progress_meter.hpp>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace FileTracking
{
    namespace
    {
        std::uint64_t bytesSentOf(std::optional<UploadProgress> const& progress)
        {
            if (!progress)
                return 0u;
            return progress->transferred ? std::max(progress->bytesSent, progress->bytesTotal) : progress->bytesSent;
        }

        std::uint64_t
        bytesTotalOf(std::optional<UploadProgress> const& progress, std::optional<std::uint64_t> const& sizeBytes)
        {
            const auto told = progress && progress->bytesTotal > 0u ? progress->bytesTotal : sizeBytes.value_or(0u);
            return std::max(told, bytesSentOf(progress));
        }

        /**
         * @brief The files an upload finished, all of them once every byte went.
         */
        std::uint64_t filesDoneOf(std::optional<UploadProgress> const& progress)
        {
            if (!progress)
                return 0u;
            if (progress->transferred)
                return std::max({progress->filesDone, progress->fileCount, std::uint64_t{1u}});
            return progress->filesDone;
        }

        /**
         * @brief The files an upload started and did not finish, at least the one it is busy with.
         */
        std::uint64_t filesRunningOf(std::optional<UploadProgress> const& progress)
        {
            if (!progress || progress->transferred)
                return 0u;
            return std::max(static_cast<std::uint64_t>(progress->inFlight.size()), std::uint64_t{1u});
        }
    }

    WatchProgressMeter::WatchProgressMeter(Ids::WatchId watchId)
        : watchId_{std::move(watchId)}
    {}

    Ids::WatchId const& WatchProgressMeter::watchId() const
    {
        return watchId_;
    }

    void WatchProgressMeter::setChangesPending(std::uint64_t count)
    {
        if (std::exchange(changesPending_, count) != count)
            dirty_ = true;
    }

    void WatchProgressMeter::setWaitingForAnswer(std::uint64_t count)
    {
        if (std::exchange(waitingForAnswer_, count) != count)
            dirty_ = true;
    }

    void WatchProgressMeter::changesEnded(std::uint64_t count)
    {
        if (count == 0u)
            return;
        burstChangesDone_ += count;
        dirty_ = true;
    }

    std::uint64_t
    WatchProgressMeter::uploadQueued(Paths::LocalPath const& path, std::optional<std::uint64_t> sizeBytes)
    {
        if (const auto previous = uploads_.find(path); previous != uploads_.end())
            endUpload(previous->second, UploadResult::NotCompleted);
        const auto generation = nextGeneration_++;
        uploads_.insert_or_assign(
            path, ActiveUpload{.sizeBytes = sizeBytes, .progress = std::nullopt, .generation = generation}
        );
        dirty_ = true;
        return generation;
    }

    void WatchProgressMeter::uploadProgressed(
        Paths::LocalPath const& path,
        UploadProgress const& progress,
        std::optional<std::uint64_t> generation
    )
    {
        const auto upload = activeUpload(path, generation);
        if (upload == uploads_.end())
            return;
        upload->second.progress = progress;
        dirty_ = true;
    }

    void WatchProgressMeter::uploadEnded(
        Paths::LocalPath const& path,
        UploadResult result,
        std::optional<std::uint64_t> generation
    )
    {
        const auto upload = activeUpload(path, generation);
        if (upload == uploads_.end())
            return;
        endUpload(upload->second, result);
        uploads_.erase(upload);
        dirty_ = true;
    }

    std::map<Paths::LocalPath, WatchProgressMeter::ActiveUpload>::iterator
    WatchProgressMeter::activeUpload(Paths::LocalPath const& path, std::optional<std::uint64_t> generation)
    {
        const auto upload = uploads_.find(path);
        if (upload == uploads_.end() || (generation && upload->second.generation != *generation))
            return uploads_.end();
        return upload;
    }

    void WatchProgressMeter::endUpload(ActiveUpload const& upload, UploadResult result)
    {
        burstBytes_ += result == UploadResult::Completed ? bytesTotalOf(upload.progress, upload.sizeBytes)
                                                         : bytesSentOf(upload.progress);
    }

    void WatchProgressMeter::dropPending()
    {
        if (isIdle())
            return;
        for (auto const& upload : std::views::values(uploads_))
            endUpload(upload, UploadResult::NotCompleted);
        uploads_.clear();
        changesPending_ = 0u;
        waitingForAnswer_ = 0u;
        dirty_ = true;
    }

    bool WatchProgressMeter::isIdle() const
    {
        return changesPending_ == 0u && waitingForAnswer_ == 0u && uploads_.empty();
    }

    bool WatchProgressMeter::finishBurst(std::uint64_t endedAt)
    {
        if (!isIdle() || (burstChangesDone_ == 0u && burstBytes_ == 0u))
            return false;
        lastSync_ = SharedData::WatchSync{
            .endedAt = endedAt,
            .changes = std::exchange(burstChangesDone_, 0u),
            .bytes = std::exchange(burstBytes_, 0u),
        };
        dirty_ = true;
        return true;
    }

    SharedData::WatchProgress WatchProgressMeter::snapshot() const
    {
        std::uint64_t filesDone = 0u;
        std::uint64_t filesRunning = 0u;
        std::uint64_t bytesDone = burstBytes_;
        std::uint64_t bytesTotal = burstBytes_;
        std::vector<SharedData::WatchFileProgress> inFlight;
        for (auto const& upload : std::views::values(uploads_))
        {
            filesDone += filesDoneOf(upload.progress);
            filesRunning += filesRunningOf(upload.progress);
            bytesDone += bytesSentOf(upload.progress);
            bytesTotal += bytesTotalOf(upload.progress, upload.sizeBytes);
            if (upload.progress && !upload.progress->transferred)
                inFlight.insert(inFlight.end(), upload.progress->inFlight.begin(), upload.progress->inFlight.end());
        }
        const auto finished = std::min(filesDone, changesPending_);
        const auto pending = changesPending_ - finished;
        const auto waitingForAnswer = std::min(waitingForAnswer_, pending);
        return SharedData::WatchProgress{
            .watchId = watchId_,
            .changesPending = pending,
            .changesDone = burstChangesDone_ + finished,
            .changesWaiting = std::max(pending - std::min(filesRunning, pending), waitingForAnswer),
            .waitingForAnswer = waitingForAnswer,
            .bytesDone = bytesDone,
            .bytesTotal = bytesTotal,
            .inFlight = std::move(inFlight),
            .lastSync = lastSync_,
        };
    }

    bool WatchProgressMeter::isDirty() const
    {
        return dirty_;
    }

    bool WatchProgressMeter::takeDirty()
    {
        return std::exchange(dirty_, false);
    }
}
