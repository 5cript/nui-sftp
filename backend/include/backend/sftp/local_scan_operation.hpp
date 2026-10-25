#pragma once

#include <ssh/file_stream.hpp>
#include <backend/sftp/operation.hpp>
#include <backend/sftp/scan_walkers.hpp>
#include <log/toast.hpp>
#include <nui/utility/move_detector.hpp>
#include <shared_data/ignore_rules.hpp>
#include <shared_data/sync/scan_node.hpp>
#include <shared_data/sync/tree_directory_walker.hpp>
#include <utility/directory_traversal.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <unordered_set>
#include <vector>
#include <memory>

class LocalScanOperation : public Operation
{
  public:
    struct ScanOperationOptions
    {
        std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
            progressCallback = [](auto, auto, auto) {};
        Paths::LocalPath localPath{};
        bool respectIgnoreFiles{false};
        /// When false, only the root directory is listed; subdirectories are not descended into.
        bool recursive{true};
        /// When true, entries whose filename starts with '.' are skipped during scan.
        bool ignoreHidden{false};
        /// When true, the operation builds a sorted @ref SharedData::Sync::ScanNode tree
        /// instead of a flat entry vector.
        bool buildTree{false};
        /** @brief Tree mode only: names ending in one of these, the temp files of transfers, stay excluded. */
        std::vector<std::string> excludedSuffixes{};
        /**
         * @brief Whether the errors the scan logs when it fails are shown as toasts, see
         *        UploadOperation::UploadOperationOptions::failureToast.
         */
        Log::Toast failureToast{Log::Toast::Show};
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

    Paths::LocalPath const& localPath() const
    {
        return localPath_;
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
            *this, walker_, recursive_, /*requireLocalNames=*/false, excludedSuffixes_, std::forward<decltype(fn)>(fn)
        );
    }

    /**
     * @brief Eject the scanned directory entries. Careful!: The internal list is moved out.
     *        Only valid when @ref buildsTree() is false. A directory that could not be listed is kept as an entry
     *        of unknown type, which no consumer creates, see @ref unreadableDirectories.
     */
    std::vector<SharedData::DirectoryEntry> ejectEntries()
    {
        auto entries = withWalkerDo(
            [](auto& walker)
            {
                return std::move(walker).ejectEntries();
            }
        );
        markUnreadable(entries);
        return entries;
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
                return ScanWalkers::rejectedPaths(walker, localPath_.toUtf8());
            }
        );
    }

    /**
     * @brief Full paths of directories below the root that could not be listed. Flat scans only, their entries are
     *        never created; the tree of a sync scan keeps them as unreadable nodes.
     */
    std::vector<std::string> const& unreadableDirectories() const noexcept
    {
        return unreadableDirectories_;
    }

    /**
     * @brief Full paths of links and junctions a flat scan on Windows leaves out, they are not uploaded from there.
     */
    std::vector<std::string> const& skippedLinks() const noexcept
    {
        return skippedLinks_;
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
    /**
     * @brief A directory below the root that vanished or denies listing is unreadable, a sync tree keeps
     *        it as such. Any other listing error fails the scan.
     */
    std::expected<std::vector<SharedData::DirectoryEntry>, Error> listingFailed(
        std::filesystem::path const& path,
        Paths::RelativePath const& relative,
        std::error_code const& errorCode
    );

    /**
     * @brief Gives the entries of @ref unreadableDirectories an unknown type.
     */
    void markUnreadable(std::vector<SharedData::DirectoryEntry>& entries) const;

  private:
    std::vector<std::string> unreadableDirectories_{};
    /** @brief The unreadable directories below the root, as relative paths. */
    std::unordered_set<std::string> unreadableRelatives_{};
    std::vector<std::string> skippedLinks_{};
    Paths::LocalPath localPath_;
    std::function<void(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)>
        progressCallback_;
    bool respectIgnoreFiles_;
    bool recursive_;
    bool ignoreHidden_;
    bool buildTree_;
    std::vector<std::string> excludedSuffixes_;
    Log::Toast failureToast_;
    bool rootScanned_{false};
    SharedData::IgnoreMatcher ignoreMatcher_;
    std::unique_ptr<Utility::BaseDirectoryWalker> walker_;
};