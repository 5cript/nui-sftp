#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <fstream>
#include <string>

class DownloadOperation : public Operation
{
  public:
    struct DownloadOperationOptions
    {
        std::function<void(std::uint64_t min, std::uint64_t max, std::uint64_t current)> progressCallback =
            [](auto, auto, auto) {};
        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
        std::string tempFileSuffix{".filepart"};
        bool mayOverwrite{false};
        bool reserveSpace{false};
        bool tryContinue{false};
        bool inheritPermissions{false};
        bool doCleanup{true};
        std::optional<std::filesystem::perms> filePermissions{std::nullopt};
        std::optional<std::filesystem::perms> directoryPermissions{std::nullopt};
        std::chrono::seconds futureTimeout{5};
    };

    SecureShell::ProcessingStrand* strand() const override
    {
        if (auto stream = fileStream_.lock(); stream)
            return stream->strand();
        return nullptr;
    }

    DownloadOperation(std::weak_ptr<SecureShell::IFileStream> fileStream, DownloadOperationOptions options);
    ~DownloadOperation() override;
    DownloadOperation(DownloadOperation const&) = delete;
    DownloadOperation(DownloadOperation&&) = delete;
    DownloadOperation& operator=(DownloadOperation const&) = delete;
    DownloadOperation& operator=(DownloadOperation&&) = delete;

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
        return SharedData::OperationType::Download;
    }

    std::filesystem::path remotePath() const
    {
        return remotePath_;
    }

    std::filesystem::path localPath() const
    {
        return localPath_;
    }

    std::expected<void, DownloadOperation::Error> cancel(bool adoptCancelState) override;

    std::expected<void, Error> prepare();
    std::expected<void, Error> finalize();

  private:
    /// Returns true if there is more data to read, false if the operation is complete.
    std::expected<bool, Error> readOnce();

    std::expected<void, Error> openOrAdoptFile(SecureShell::IFileStream& stream);

    void cleanup();

  private:
    std::weak_ptr<SecureShell::IFileStream> fileStream_;
    std::filesystem::path remotePath_;
    std::filesystem::path localPath_;
    std::string tempFileSuffix_;
    std::function<void(std::uint64_t min, std::uint64_t max, std::uint64_t current)> progressCallback_;
    bool mayOverwrite_;
    bool reserveSpace_;
    bool tryContinue_;
    bool inheritPermissions_;
    bool doCleanup_;
    std::optional<std::filesystem::perms> filePermissions_;
    std::optional<std::filesystem::perms> directoryPermissions_;
    std::ofstream localFile_;
    std::uint64_t fileSize_;
    std::chrono::seconds futureTimeout_;
    std::array<char, 8192> buffer_;
};