#pragma once

#include <ssh/file_stream.hpp>
#include <backend/sftp/operation.hpp>
#include <nui/utility/move_detector.hpp>
#include <utility/directory_traversal.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <vector>
#include <memory>

class LocalScanOperation : public Operation
{
  public:
    struct ScanOperationOptions
    {
        std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
            progressCallback = [](auto, auto, auto) {};
        std::filesystem::path localPath{};
    };

    LocalScanOperation(ScanOperationOptions options);
    ~LocalScanOperation() override;
    LocalScanOperation(LocalScanOperation const&) = delete;
    LocalScanOperation(LocalScanOperation&&) = delete;
    LocalScanOperation& operator=(LocalScanOperation const&) = delete;
    LocalScanOperation& operator=(LocalScanOperation&&) = delete;

    SecureShell::ProcessingStrand* strand() const override
    {
        return nullptr;
    }

    std::expected<WorkStatus, Error> work() override;

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::LocalScan;
    }

    bool isBarrier() const noexcept override
    {
        return true;
    }

    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    std::filesystem::path localPath() const
    {
        return localPath_;
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
            walker_ = std::make_unique<WalkerType>(localPath_, std::move(scan));
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
    std::filesystem::path localPath_;
    std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
        progressCallback_;
    std::unique_ptr<Utility::BaseDirectoryWalker> walker_;
};