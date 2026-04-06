#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/sftp_session.hpp>

#include <chrono>
#include <filesystem>

class RenameOperation : public Operation
{
  public:
    struct RenameOperationOptions
    {
        std::filesystem::path sourcePath{};
        std::filesystem::path destinationPath{};
        std::chrono::seconds futureTimeout{5};
    };

    RenameOperation(SecureShell::SftpSession& sftp, RenameOperationOptions options);
    ~RenameOperation() override = default;

    RenameOperation(RenameOperation const&) = delete;
    RenameOperation(RenameOperation&&) = delete;
    RenameOperation& operator=(RenameOperation const&) = delete;
    RenameOperation& operator=(RenameOperation&&) = delete;

    SecureShell::ProcessingStrand* strand() const override;

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

    std::filesystem::path const& sourcePath() const
    {
        return sourcePath_;
    }

    std::filesystem::path const& destinationPath() const
    {
        return destinationPath_;
    }

  private:
    SecureShell::SftpSession* sftp_;
    std::filesystem::path sourcePath_;
    std::filesystem::path destinationPath_;
    std::chrono::seconds futureTimeout_;
};
