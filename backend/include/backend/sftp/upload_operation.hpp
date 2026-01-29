#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <fstream>
#include <string>

class UploadOperation : public Operation
{
  public:
    struct UploadOperationOptions
    {
        std::function<void(std::uint64_t min, std::uint64_t max, std::uint64_t current)> progressCallback =
            [](auto, auto, auto) {};
        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
        std::string tempFileSuffix{".filepart"};
        bool mayOverwrite{false};
        bool tryContinue{false};
        bool inheritPermissions{false};
        std::optional<std::filesystem::perms> filePermissions{std::nullopt};
        std::optional<std::filesystem::perms> directoryPermissions{std::nullopt};
        std::chrono::seconds futureTimeout{5};
    };

    SecureShell::ProcessingStrand* strand() const override;

    UploadOperation(SecureShell::SftpSession& sftp, UploadOperationOptions options);
    ~UploadOperation() override;
    UploadOperation(UploadOperation const&) = delete;
    UploadOperation(UploadOperation&&) = delete;
    UploadOperation& operator=(UploadOperation const&) = delete;
    UploadOperation& operator=(UploadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;

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
        return SharedData::OperationType::Upload;
    }

    std::filesystem::path remotePath() const
    {
        return remotePath_;
    }

    std::filesystem::path localPath() const
    {
        return localPath_;
    }

    std::size_t totalSize() const
    {
        return totalSize_;
    }

    std::expected<void, UploadOperation::Error> cancel(bool adoptCancelState) override;

    std::expected<void, Error> prepare();
    std::expected<void, Error> finalize();

  private:
    /// Returns true if there is more data to write, false if the operation is complete.
    std::expected<bool, Error> writeOnce();

    std::expected<void, Error> openOrAdoptFile();

    std::filesystem::perms determinePerms(std::filesystem::perms localPerms) const;

    void cleanup();

  private:
    SecureShell::SftpSession* sftp_;
    std::weak_ptr<SecureShell::IFileStream> fileStream_;
    std::filesystem::path remotePath_;
    std::filesystem::path localPath_;
    std::string tempFileSuffix_;
    std::function<void(std::uint64_t min, std::uint64_t max, std::uint64_t current)> progressCallback_;
    bool mayOverwrite_;
    bool tryContinue_;
    bool inheritPermissions_;
    std::optional<std::filesystem::perms> filePermissions_;
    std::optional<std::filesystem::perms> directoryPermissions_;
    std::ifstream localFile_;
    std::chrono::seconds futureTimeout_;
    std::size_t leftToUpload_{0};
    std::size_t totalSize_{0};
    std::array<char, 8192> buffer_{};
};