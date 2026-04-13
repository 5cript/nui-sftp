#pragma once

#include <ssh/file_stream.hpp>
#include <backend/sftp/operation.hpp>
#include <nui/utility/move_detector.hpp>
#include <shared_data/ignore_rules.hpp>
#include <utility/directory_traversal.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>

class ScanOperation : public Operation
{
  public:
    struct ScanOperationOptions
    {
        std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
            progressCallback = [](auto, auto, auto) {};
        std::filesystem::path remotePath{};
        std::chrono::seconds futureTimeout{5};
        bool respectIgnoreFiles{false};
    };

    SecureShell::ProcessingStrand* strand() const override;

    ScanOperation(SecureShell::SftpSession& sftp, ScanOperationOptions options);
    ~ScanOperation() override;
    ScanOperation(ScanOperation const&) = delete;
    ScanOperation(ScanOperation&&) = delete;
    ScanOperation& operator=(ScanOperation const&) = delete;
    ScanOperation& operator=(ScanOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::Scan;
    }

    bool isBarrier() const noexcept override
    {
        return true;
    }

    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    std::filesystem::path remotePath() const
    {
        return remotePath_;
    }

    std::expected<void, Error> cancel(bool adoptCancelState) override;

    auto withWalkerDo(auto&& fn)
    {
        auto scan = [this](std::filesystem::path const& path)
        {
            return scanner(path);
        };
        using WalkerType = Utility::DeepDirectoryWalker<SharedData::DirectoryEntry, Error, decltype(scan), true>;
        if (!walker_)
            walker_ = std::make_unique<WalkerType>(remotePath_, std::move(scan));
        return fn(static_cast<WalkerType&>(*walker_));
    }

    /**
     * @brief Eject the scanned directory entries. Careful!: The internal list is moved out.
     *
     * @return std::vector<SharedData::DirectoryEntry>
     */
    std::vector<SharedData::DirectoryEntry> ejectEntries()
    {
        return withWalkerDo(
            [](auto& walker)
            {
                return std::move(walker).ejectEntries();
            }
        );
    }

    std::uint64_t totalBytes() const;

    std::expected<std::vector<SharedData::DirectoryEntry>, Error> scanner(std::filesystem::path const& path);

  private:
    SecureShell::SftpSession* sftp_;
    std::filesystem::path remotePath_;
    std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
        progressCallback_;
    std::chrono::seconds futureTimeout_;
    bool respectIgnoreFiles_;
    SharedData::IgnoreMatcher ignoreMatcher_;
    std::unique_ptr<Utility::BaseDirectoryWalker> walker_;
};