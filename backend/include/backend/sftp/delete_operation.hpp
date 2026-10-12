#pragma once

#include <backend/sftp/operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <backend/sftp/scan_phase.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>
#include <paths/remote_path.hpp>

#include <filesystem>
#include <fstream>
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

    DeleteOperation(SecureShell::SftpSession& sftp, DeleteOperationOptions options);
    ~DeleteOperation() override;
    DeleteOperation(DeleteOperation const&) = delete;
    DeleteOperation(DeleteOperation&&) = delete;
    DeleteOperation& operator=(DeleteOperation const&) = delete;
    DeleteOperation& operator=(DeleteOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;

    void setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t /* totalBytes */);

    /**
     * @brief Deletes exactly @p targets, last first, without a scan. For a bulk delete of files.
     */
    void setTargets(std::vector<Paths::RemotePath> targets);

    /**
     * @brief Scans with @p scan before deleting; every scanned entry is deleted, deepest first.
     */
    void scanFirst(std::unique_ptr<ScanOperation> scan);

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

  private:
    std::expected<void, DeleteOperation::Error> removeOnce(Paths::RemotePath const& path);

  private:
    SecureShell::SftpSession* sftp_;
    Paths::RemotePath remotePath_;
    std::function<void(std::string const&, std::uint64_t, std::uint64_t)> filesRemovedProgress_;
    std::chrono::seconds futureTimeout_;
    std::size_t deletedThusFar_{0};
    std::size_t totalToDelete_{0};
    /** @brief Deleted from the back, so a scanned directory goes after its content. */
    std::vector<Paths::RemotePath> targets_{};
    /** @brief A scanned entry was not a valid path within remotePath_, nothing may be deleted. */
    std::optional<std::string> invalidScanEntry_{};
    std::unique_ptr<ScanPhaseDriver> scanPhase_{};
    bool scanDelivered_{false};
    bool recursive_{false};
};