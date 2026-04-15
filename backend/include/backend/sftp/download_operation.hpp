#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/utility/move_detector.hpp>
#include <persistence/state/sftp_options.hpp>

#include <filesystem>
#include <fstream>
#include <string>

class DownloadOperation : public Operation
{
  public:
    struct DownloadOperationOptions
    {
        std::function<void(
            std::uint64_t min,
            std::uint64_t max,
            std::uint64_t current,
            std::make_signed_t<std::size_t> bytesPerSecond
        )>
            progressCallback = [](auto, auto, auto, auto) {};
        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
        std::string tempFileSuffix{".filepart"};
        bool mayOverwrite{false};
        bool reserveSpace{false};
        bool tryContinue{false};
        bool inheritPermissions{false};
        bool doCleanup{true};
        bool bigFileOptimized{false};
        std::optional<std::filesystem::perms> filePermissions{std::nullopt};
        std::optional<std::filesystem::perms> directoryPermissions{std::nullopt};
        std::chrono::seconds futureTimeout{5};
        Persistence::SymlinkHandling symlinkHandling{Persistence::SymlinkHandling::AsSymlink};
        std::optional<SharedData::DirectoryEntry> entry{std::nullopt};
        /** @brief Create every missing parent directory of @ref localPath before opening the
         *         local file.  Off by default — enable for sync flows whose target subtree
         *         may not yet exist locally.
         */
        bool createMissingDirectories{false};
    };

    SecureShell::ProcessingStrand* strand() const override
    {
        if (auto stream = fileStream_.lock(); stream)
            return stream->strand();
        return sftp_ ? sftp_->strand() : nullptr;
    }

    DownloadOperation(SecureShell::SftpSession& sftp, DownloadOperationOptions options);
    ~DownloadOperation() override;
    DownloadOperation(DownloadOperation const&) = delete;
    DownloadOperation(DownloadOperation&&) = delete;
    DownloadOperation& operator=(DownloadOperation const&) = delete;
    DownloadOperation& operator=(DownloadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;
    std::expected<WorkStatus, Error> workInStrand() override;

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
        return options_.remotePath;
    }

    std::filesystem::path localPath() const
    {
        return options_.localPath;
    }

    std::uint64_t totalSize() const
    {
        if (options_.entry)
            return options_.entry->size;
        return 0;
    }

    std::expected<void, DownloadOperation::Error> cancel(bool adoptCancelState) override;

    void pause(bool doPause) override;

    std::expected<void, Error> prepare();
    std::expected<void, Error> finalize();
    std::expected<void, Error> finalizeInStrand();

  private:
    /// Returns true if there is more data to read, false if the operation is complete.
    std::expected<bool, Error> readOnce();
    std::expected<bool, Error> readOnceInStrand();

    std::expected<void, Error> handleSymlink();
    std::expected<void, Error> handleSymlinkInStrand();

    bool commitBufferToFile(SecureShell::IFileStream::SignedSizeType bytesRead);

    /// Runs the remote-heavy portion of prepare() under a single strand umbrella.
    std::expected<void, Error> prepareInStrand();

    std::expected<void, Error> openOrAdoptFileInStrand(SecureShell::IFileStream& stream);

    std::expected<SecureShell::SftpSession::DeepLinkResult, Error>
    readSymlinkInStrand(std::filesystem::path const& remoteFullPath);

    void cleanup();

  private:
    SecureShell::SftpSession* sftp_;
    std::weak_ptr<SecureShell::IFileStream> fileStream_;
    DownloadOperationOptions options_;
    std::ofstream localFile_;
    std::array<char, 16384> buffer_;
    std::shared_ptr<SecureShell::AsyncTransferContext> asyncTransferContext_;
};