#pragma once

#include <ssh/file_stream.hpp>
#include <backend/sftp/operation.hpp>
#include <nui/utility/move_detector.hpp>
#include <shared_data/ignore_rules.hpp>
#include <shared_data/sync/scan_node.hpp>
#include <shared_data/sync/tree_directory_walker.hpp>
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
        bool respectIgnoreFiles{false};
        /// When false, only the root directory is listed; subdirectories are not descended into.
        bool recursive{true};
        /// When true, entries whose filename starts with '.' are skipped during scan.
        bool ignoreHidden{false};
        /// When true, the operation builds a sorted @ref SharedData::Sync::ScanNode tree
        /// instead of a flat entry vector.
        bool buildTree{false};
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

    bool usesStrand() const noexcept override
    {
        return false;
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
     * @brief Variant of @ref withWalkerDo that drives a sorted ScanNode tree walker.
     *        Only call when @ref ScanOperationOptions::buildTree was set.
     */
    auto withTreeWalkerDo(auto&& fn)
    {
        auto scan = [this](std::filesystem::path const& path)
        {
            return scanner(path);
        };
        using WalkerType = SharedData::Sync::TreeDirectoryWalker<Error, decltype(scan), true>;
        if (!walker_)
            walker_ = std::make_unique<WalkerType>(localPath_, std::move(scan));
        return fn(static_cast<WalkerType&>(*walker_));
    }

    /**
     * @brief Eject the scanned directory entries. Careful!: The internal list is moved out.
     *        Only valid when @ref buildsTree() is false.
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

    /**
     * @brief Eject the scanned node tree. Only valid when @ref buildsTree() is true.
     */
    SharedData::Sync::ScanNode ejectScanTree()
    {
        return withTreeWalkerDo(
            [](auto& walker)
            {
                return std::move(walker).ejectTree();
            }
        );
    }

    bool buildsTree() const noexcept
    {
        return buildTree_;
    }

    std::uint64_t totalBytes() const;

    std::expected<std::vector<SharedData::DirectoryEntry>, Error> scanner(std::filesystem::path const& path);

  private:
    std::filesystem::path localPath_;
    std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
        progressCallback_;
    bool respectIgnoreFiles_;
    bool recursive_;
    bool ignoreHidden_;
    bool buildTree_;
    bool rootScanned_{false};
    SharedData::IgnoreMatcher ignoreMatcher_;
    std::unique_ptr<Utility::BaseDirectoryWalker> walker_;
};