#pragma once

#include <backend/sftp/operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <backend/sftp/scan_phase.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/file_operations/compared_state.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

class DeleteOperation : public Operation
{
  public:
    struct DeleteOperationOptions
    {
        std::function<void(std::string const&, std::uint64_t current, std::uint64_t max)> filesRemovedProgress =
            [](std::string const&, auto, auto) {};
        /** @brief What is deleted, or for a recursive delete the root every scanned entry must lie within. */
        Paths::RemotePath remotePath{};
        std::chrono::seconds futureTimeout{5};
        bool recursive{false};
    };

    SecureShell::ProcessingStrand* strand() const override;

    // See ScanOperation for rationale — opt out of the queue's batched strand umbrella
    // until this op is converted to the *InStrand style.
    bool usesStrand() const noexcept override
    {
        return false;
    }

    /**
     * @brief One item to delete.
     */
    struct Target
    {
        Paths::RemotePath path{};
        /**
         * @brief What a sync compare saw there. An item that no longer is what it saw is not deleted but fails with
         *        ChangedSinceCompare. Unset outside of sync.
         */
        std::optional<SharedData::ComparedState> compared{};
    };

    DeleteOperation(std::weak_ptr<SecureShell::SftpSession> sftp, DeleteOperationOptions options);
    ~DeleteOperation() override;
    DeleteOperation(DeleteOperation const&) = delete;
    DeleteOperation(DeleteOperation&&) = delete;
    DeleteOperation& operator=(DeleteOperation const&) = delete;
    DeleteOperation& operator=(DeleteOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;

    /**
     * @brief The scanned content of remotePath, see @ref appendScanResult.
     */
    void setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t /* totalBytes */);

    /**
     * @brief Adds the entries of a scan of @p root as targets. The first entry is @p root itself, every other one
     *        lies below it through its parents. A second root, an invalid name or the root directory refuses the
     *        scan, then nothing is deleted.
     */
    void appendScanResult(Paths::RemotePath const& root, std::vector<SharedData::DirectoryEntry> const& entries);

    /**
     * @brief Deletes exactly @p targets, last first, without a scan. For the files of a bulk delete.
     */
    void setTargets(std::vector<Target> targets);

    /**
     * @brief Scans with @p scan before deleting; every scanned entry is deleted, deepest first. One operation can
     *        take several roots. A root whose scan fails is a failed entry and the other roots are still deleted.
     */
    void addScanRoot(std::unique_ptr<ScanOperation> scan);

    /**
     * @brief Counts @p target as an item that could not be deleted, like one refused before it was queued.
     */
    void addFailedTarget(Paths::RemotePath const& target, Error error);

    bool scanning() const noexcept
    {
        return scanPhase_ != nullptr;
    }

    bool isBarrier() const noexcept override
    {
        return scanPhase_ != nullptr;
    }

    /**
     * @brief How much parallel work does this operation do.
     *
     * @param parallel Maximum parallelism allowed.
     * @return The amount of parallel work that can be done maxed by parallel parameter.
     */
    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::Delete;
    }

    Paths::RemotePath const& remotePath() const
    {
        return remotePath_;
    }

    std::expected<void, DeleteOperation::Error> cancel(bool adoptCancelState) override;

    /**
     * @brief Items that could not be deleted, with why. The rest was still deleted.
     */
    std::vector<std::pair<std::string, Error>> const& getFailed() const noexcept
    {
        return failedEntries_;
    }

  private:
    std::expected<void, DeleteOperation::Error> removeOnce(Target const& target);

  private:
    std::weak_ptr<SecureShell::SftpSession> sftp_;
    Paths::RemotePath remotePath_;
    std::function<void(std::string const&, std::uint64_t, std::uint64_t)> filesRemovedProgress_;
    std::chrono::seconds futureTimeout_;
    std::size_t deletedThusFar_{0};
    std::size_t totalToDelete_{0};
    /** @brief Targets whose removal failed, the operation fails when that is all of them and nothing else. */
    std::size_t removalFailures_{0};
    /** @brief Deleted from the back, so a scanned directory goes after its content. */
    std::vector<Target> targets_{};
    /** @brief Why a scan result was refused, nothing may be deleted then. */
    std::optional<std::string> invalidScanEntry_{};
    std::vector<std::pair<std::string, Error>> failedEntries_{};
    std::unique_ptr<ScanPhase<ScanOperation>> scanPhase_{};
    bool scanDelivered_{false};
    bool recursive_{false};
};