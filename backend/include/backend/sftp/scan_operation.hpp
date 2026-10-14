#pragma once

#include <ssh/file_stream.hpp>
#include <backend/sftp/operation.hpp>
#include <backend/sftp/scan_walkers.hpp>
#include <nui/utility/move_detector.hpp>
#include <shared_data/ignore_rules.hpp>
#include <shared_data/sync/scan_node.hpp>
#include <shared_data/sync/tree_directory_walker.hpp>
#include <utility/directory_traversal.hpp>
#include <paths/paths.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <cstdint>

class ScanOperation : public Operation
{
  public:
    struct ScanOperationOptions
    {
        std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
            progressCallback = [](auto, auto, auto) {};
        Paths::RemotePath remotePath{};
        std::chrono::seconds futureTimeout{5};
        bool respectIgnoreFiles{false};
        /// When false, only the root directory is listed; subdirectories are not descended into.
        bool recursive{true};
        /// When true, entries whose filename starts with '.' are skipped during scan.
        bool ignoreHidden{false};
        /// When true, the operation builds a sorted @ref SharedData::Sync::ScanNode tree
        /// instead of a flat entry vector.  Only one shape is active at a time; use
        /// @ref ejectScanTree() in that case and @ref ejectEntries() otherwise.
        bool buildTree{false};
        /** @brief Tree mode only: names ending in one of these, the temp files of transfers, stay excluded. */
        std::vector<std::string> excludedSuffixes{};
    };

    SecureShell::ProcessingStrand* strand() const override;

    // work() here still waits on futures pushed to the SFTP strand; running it inside
    // the queue's batched strand umbrella would self-deadlock. Opt out of batching until
    // this op is converted to the *InStrand style.
    bool usesStrand() const noexcept override
    {
        return false;
    }

    ScanOperation(std::weak_ptr<SecureShell::SftpSession> sftp, ScanOperationOptions options);
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

    Paths::RemotePath const& remotePath() const
    {
        return remotePath_;
    }

    std::expected<void, Error> cancel(bool adoptCancelState) override;

    auto withWalkerDo(auto&& fn)
    {
        return ScanWalkers::withFlatWalkerDo(*this, walker_, std::forward<decltype(fn)>(fn));
    }

    /**
     * @brief Variant of @ref withWalkerDo that drives a sorted ScanNode tree walker.
     *        Only call when @ref ScanOperationOptions::buildTree was set.
     */
    auto withTreeWalkerDo(auto&& fn)
    {
        return ScanWalkers::withTreeWalkerDo(
            *this, walker_, recursive_, /*requireLocalNames=*/true, excludedSuffixes_, std::forward<decltype(fn)>(fn)
        );
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
     * @brief Full paths of listed entries whose name is not a valid path segment, for display. Flat scans only,
     *        the tree of a sync scan keeps them as invalid name nodes.
     */
    std::vector<std::string> rejectedPaths()
    {
        return withWalkerDo(
            [this](auto& walker)
            {
                return ScanWalkers::rejectedPaths(walker, remotePath_.value());
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

    /**
     * @brief Lists the directory at @p relative below the scanned root.
     */
    std::expected<std::vector<SharedData::DirectoryEntry>, Error> scanner(Paths::RelativePath const& relative);

  private:
    std::weak_ptr<SecureShell::SftpSession> sftp_;
    Paths::RemotePath remotePath_;
    std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
        progressCallback_;
    std::chrono::seconds futureTimeout_;
    bool respectIgnoreFiles_;
    bool recursive_;
    bool ignoreHidden_;
    bool buildTree_;
    std::vector<std::string> excludedSuffixes_;
    bool rootScanned_{false};
    SharedData::IgnoreMatcher ignoreMatcher_;
    std::unique_ptr<Utility::BaseDirectoryWalker> walker_;
};