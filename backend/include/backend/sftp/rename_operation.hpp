#pragma once

#include <backend/sftp/operation.hpp>
#include <remote_io/remote_file_system.hpp>
#include <paths/remote_path.hpp>

#include <chrono>
#include <filesystem>
#include <memory>

class RenameOperation : public Operation
{
  public:
    struct RenameOperationOptions
    {
        Paths::RemotePath sourcePath{};
        Paths::RemotePath destinationPath{};
        std::chrono::seconds futureTimeout{5};
    };

    RenameOperation(std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem, RenameOperationOptions options);
    ~RenameOperation() override = default;

    RenameOperation(RenameOperation const&) = delete;
    RenameOperation(RenameOperation&&) = delete;
    RenameOperation& operator=(RenameOperation const&) = delete;
    RenameOperation& operator=(RenameOperation&&) = delete;

    RemoteIo::ProcessingStrand* strand() const override;

    // See ScanOperation for rationale — opt out of the queue's batched strand umbrella
    // until this op is converted to the *InStrand style.
    bool usesStrand() const noexcept override
    {
        return false;
    }

    std::expected<WorkStatus, Error> work() override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;

    bool isBarrier() const noexcept override
    {
        return false;
    }

    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::Rename;
    }

    Paths::RemotePath const& sourcePath() const
    {
        return sourcePath_;
    }

    Paths::RemotePath const& destinationPath() const
    {
        return destinationPath_;
    }

  private:
    std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem_;
    Paths::RemotePath sourcePath_;
    Paths::RemotePath destinationPath_;
    std::chrono::seconds futureTimeout_;
};
