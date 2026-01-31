#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>

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
        std::filesystem::path remotePath{};
        std::chrono::seconds futureTimeout{5};
        bool recursive{false};
    };

    SecureShell::ProcessingStrand* strand() const override;

    DeleteOperation(SecureShell::SftpSession& sftp, DeleteOperationOptions options);
    ~DeleteOperation() override;
    DeleteOperation(DeleteOperation const&) = delete;
    DeleteOperation(DeleteOperation&&) = delete;
    DeleteOperation& operator=(DeleteOperation const&) = delete;
    DeleteOperation& operator=(DeleteOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;

    void setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t /* totalBytes */);

    bool isBarrier() const noexcept override
    {
        return false;
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

    std::filesystem::path remotePath() const
    {
        return remotePath_;
    }

    std::expected<void, DeleteOperation::Error> cancel(bool adoptCancelState) override;

  private:
    std::expected<void, DeleteOperation::Error> removeOnce(std::filesystem::path const& path);

  private:
    SecureShell::SftpSession* sftp_;
    std::filesystem::path remotePath_;
    std::function<void(std::string const&, std::uint64_t, std::uint64_t)> filesRemovedProgress_;
    std::chrono::seconds futureTimeout_;
    std::size_t deletedThusFar_{0};
    std::size_t totalToDelete_{0};
    std::vector<SharedData::DirectoryEntry> entries_{};
    bool recursive_{false};
};