#pragma once

#include <backend/sftp/operation.hpp>
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sftp/scan_phase.hpp>
#include <ssh/sftp_session.hpp>
#include <shared_data/directory_entry.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <paths/paths.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

/**
 * @brief Receives the bulk's progress; operationId is left for the queue to fill in.
 */
using BulkProgressCallback = std::function<void(SharedData::BulkProgress)>;

/**
 * @brief Shared driver for bulk download and bulk upload.
 *
 * Owns the entry list, the in-flight child transfers, byte accounting and progress emission.
 * Subclasses only supply the direction specific pieces: creating directories, building a child
 * operation for one entry and resolving paths.
 *
 * Runs entirely on the SFTP processing thread (usesStrand() is true); children are stepped via
 * workInStrand() so several files move at once without any blocking future round trips.
 */
class BulkTransferOperation : public Operation
{
  public:
    struct CommonOptions
    {
        BulkProgressCallback overallProgressCallback = [](SharedData::BulkProgress const&) {};
        /** @brief Root of a scanned tree on the server, only a label for a flat list. */
        Paths::RemotePath remotePath{};
        /** @brief Root of a scanned tree on this machine, only a label for a flat list. */
        Paths::LocalPath localPath{};
        bool failFast{false};
        // Upper bound of files transferred at once. The queue may grant fewer slots per step.
        int concurrency{1};
        std::chrono::seconds futureTimeout{5};
        // Minimum gap between two progress emissions; start and completion always emit.
        std::chrono::milliseconds progressEmitInterval{100};
    };

    /**
     * @brief Both ends of one entry.
     */
    struct EntryPaths
    {
        Paths::RemotePath remote{};
        Paths::LocalPath local{};
        /** @brief This entry's destination may be replaced even when the bulk's options do not allow it. */
        bool mayOverwrite{false};
        /** @brief See SharedData::BulkUploadEntry::comparedDestination, kept by a resume. */
        std::optional<SharedData::ComparedState> comparedDestination{};
    };

    ~BulkTransferOperation() override;
    BulkTransferOperation(BulkTransferOperation const&) = delete;
    BulkTransferOperation(BulkTransferOperation&&) = delete;
    BulkTransferOperation& operator=(BulkTransferOperation const&) = delete;
    BulkTransferOperation& operator=(BulkTransferOperation&&) = delete;

    /**
     * @brief Off-strand entry point for tests and standalone callers. Hops onto the processing
     *        thread and runs one @ref workInStrand step.
     */
    std::expected<WorkStatus, Error> work() override;
    std::expected<WorkStatus, Error> workInStrand() override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;
    void pause(bool doPause) override;
    SecureShell::ProcessingStrand* strand() const override;

    // The scan phase blocks on SFTP futures, so it runs off the strand like a standalone scan.
    bool usesStrand() const noexcept override
    {
        return scanPhase_ == nullptr;
    }

    bool isBarrier() const noexcept override
    {
        return scanPhase_ != nullptr;
    }

    /**
     * @brief Slots this bulk would use: bounded by the queue's offer, the configured concurrency
     *        and the amount of remaining work, never below 1.
     */
    int parallelWorkDoable(int parallel) const noexcept override;
    void setParallelBudget(int slots) override;

    /**
     * @brief Feeds the entries of a completed tree scan. entries[0] must be the root directory.
     */
    void setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes);

    /**
     * @brief Runs @p scan before transferring; the scanned tree becomes this bulk's entries.
     *
     * @tparam ScanT ScanOperation for downloads, LocalScanOperation for uploads.
     */
    template <typename ScanT>
    void scanFirst(std::unique_ptr<ScanT> scan)
    {
        auto phase = std::make_unique<ScanPhase<ScanT>>(
            [this](ScanT& finished)
            {
                const auto scannedBytes = finished.totalBytes();
                addRejectedEntries(finished.rejectedPaths());
                if constexpr (requires { finished.unreadableDirectories(); })
                    addUnreadableDirectories(finished.unreadableDirectories());
                if constexpr (requires { finished.skippedLinks(); })
                    addSkippedLinks(finished.skippedLinks());
                setScanResult(finished.ejectEntries(), scannedBytes);
            }
        );
        phase->addRoot(std::move(scan));
        scanPhase_ = std::move(phase);
    }

    bool scanning() const noexcept
    {
        return scanPhase_ != nullptr;
    }

    /**
     * @brief Records entries the scan refused because of their name as failed, they are never transferred.
     */
    void addRejectedEntries(std::vector<std::string> const& paths);

    /**
     * @brief Records links the scan left out as failed, they are not transferred.
     */
    void addSkippedLinks(std::vector<std::string> const& paths);

    /**
     * @brief Records directories the scan could not list as failed, their content is not transferred.
     */
    void addUnreadableDirectories(std::vector<std::string> const& paths);

    /**
     * @brief The entries that were skipped, by their source path.
     */
    std::vector<std::pair<std::string, Error>> getFailed() const;

    /**
     * @brief The entries that were skipped by their index, and why.
     */
    std::vector<SharedData::IndexedFailure> const& indexedFailures() const noexcept
    {
        return indexedFailures_;
    }

    std::size_t inFlightCount() const noexcept
    {
        return slots_.size();
    }

    /**
     * @brief The entries are known: the scan delivered them or a flat list was set.
     */
    bool hasEntries() const noexcept
    {
        return entrySource_ != EntrySource::None;
    }

    /**
     * @brief The indices of the entries a resume has to do again: every entry that was neither transferred, nor
     *        created as a directory, nor skipped for its type. Failed and in flight entries are among them.
     */
    std::vector<std::size_t> unfinishedEntryIndices() const;

    /**
     * @brief Counts @p progress, what an earlier connection finished of this bulk, as done: its entries and bytes
     *        are part of the totals and of the progress from the start. Call it after the entries are set.
     */
    void countEarlierProgress(BulkResumeProgress progress);

    /**
     * @brief What a resume of this bulk leaves out: the entries it finished, without the root of a tree, and the
     *        bytes of their files, with what an earlier connection finished.
     */
    BulkResumeProgress finishedProgress() const;

    /**
     * @brief What an earlier connection finished of this bulk, see @ref countEarlierProgress.
     */
    BulkResumeProgress earlierProgress() const noexcept
    {
        return earlierProgress_;
    }

    std::uint64_t completedCount() const noexcept
    {
        return completedEntries_;
    }

    int concurrency() const noexcept
    {
        return common_.concurrency;
    }

  protected:
    BulkTransferOperation(std::weak_ptr<SecureShell::SftpSession> sftp, CommonOptions common);

    /**
     * @brief Flat-list mode: every entry is a file or a directory created without content, @p paths holds both of
     *        its ends (index aligned). A directory that cannot be created fails only its own entry.
     */
    void setPrescannedEntries(
        std::vector<SharedData::DirectoryEntry> entries,
        std::vector<EntryPaths> paths,
        std::uint64_t totalBytes
    );

    bool prescanned() const noexcept
    {
        return entrySource_ == EntrySource::Flat;
    }

    /**
     * @brief Both ends of the entry at @p entryIndex. In tree mode the entry's names are joined below the roots
     *        with @ref Detail::treeEntryPaths.
     */
    std::expected<EntryPaths, Error> entryPaths(std::size_t entryIndex) const;

    /**
     * @brief Path shown to the user for the entry, the remote side for both directions.
     */
    std::string displayPath(std::size_t entryIndex) const;

    /**
     * @brief The source path of the entry, the key a failure is reported under.
     */
    std::string failureSource(std::size_t entryIndex) const;

    /**
     * @brief The source of @p paths, the key a failure is reported under.
     */
    std::string failureSource(EntryPaths const& paths) const;

    /**
     * @brief The entry below the source root by its names as listed, for entries without valid paths.
     */
    std::string describeEntry(std::size_t entryIndex) const;

    std::vector<SharedData::DirectoryEntry> const& entries() const noexcept
    {
        return entries_;
    }

    /**
     * @brief The session the children are given, held weakly like the bulk's own.
     */
    std::weak_ptr<SecureShell::SftpSession> const& sftp() const noexcept
    {
        return sftp_;
    }

    CommonOptions const& common() const noexcept
    {
        return common_;
    }

    /**
     * @brief Tree mode only: creates the bulk's root directory from entries()[0].
     */
    virtual std::expected<void, Error>
    prepareRootInStrand(SecureShell::SftpSession& sftp, SharedData::DirectoryEntry const& root) = 0;

    /**
     * @brief Creates the directory described by the entry at @p entryIndex.
     */
    virtual std::expected<void, Error>
    createDirectoryInStrand(SecureShell::SftpSession& sftp, std::size_t entryIndex, EntryPaths const& paths) = 0;

    /**
     * @brief Builds the child transfer for the file at @p entryIndex.
     *
     * @param onProgress Must be wired to the child's progress callback as (current, max).
     */
    virtual std::unique_ptr<Operation> makeChild(
        std::size_t entryIndex,
        EntryPaths const& paths,
        std::function<void(std::uint64_t current, std::uint64_t max)> onProgress
    ) = 0;

    /**
     * @brief Whether a failed child may be recorded and skipped instead of failing the bulk.
     */
    virtual bool isSkippableError(Error const& error) const = 0;

    /**
     * @brief Whether a directory that could not be created may be recorded and skipped instead of failing the bulk.
     *        Its descendants then fail on their own.
     */
    virtual bool isSkippableDirectoryError(Error const&) const
    {
        return false;
    }

    /**
     * @brief Runs once every entry is done, before the bulk completes. A failure fails the bulk.
     */
    virtual std::expected<void, Error> finishInStrand()
    {
        return {};
    }

    /**
     * @brief Log prefix, e.g. "BulkDownloadOperation".
     */
    virtual std::string_view logName() const noexcept = 0;

    /**
     * @brief The remote side is the source, as for a download.
     */
    virtual bool sourceIsRemote() const noexcept = 0;

  private:
    struct Slot
    {
        std::size_t entryIndex;
        /** @brief Both ends of the entry, built once when it starts. */
        EntryPaths paths;
        std::unique_ptr<Operation> child;
        std::uint64_t bytes{0};
        std::uint64_t totalBytes{0};
        bool finished{false};
    };

    std::expected<WorkStatus, Error> start(SecureShell::SftpSession& sftp);
    std::expected<WorkStatus, Error> step(SecureShell::SftpSession& sftp);
    std::expected<void, Error> fillSlots(SecureShell::SftpSession& sftp);
    std::expected<void, Error> stepSlots();
    /**
     * @brief Counts the entry of @p slot as done, as @p transferred or as a failure recorded for the user.
     */
    void finishSlot(Slot& slot, bool transferred);
    void cancelChildrenInStrand(bool adoptCancelState);
    Slot* slotFor(std::size_t entryIndex) noexcept;
    std::uint64_t fileCount() const noexcept;
    std::uint64_t bytesNow() const noexcept;
    void emitProgress(bool force);
    static bool isBufferShortage(Error const& error) noexcept;

    /**
     * @brief Update and return the bulk-level bytes/second.
     * @param bytesNow Cumulative bytes transferred so far (completed files + in-flight bytes).
     * @return Signed bps value to propagate to the frontend. Sampled on a fixed cadence and
     *         smoothed; between samples the last value is returned unchanged.
     */
    std::int64_t updateBulkBytesPerSecond(std::uint64_t bytesNow);

  private:
    /**
     * @brief Where the entries came from. A scanned tree always holds its root, so an empty one
     *        means the scan never delivered.
     */
    enum class EntrySource
    {
        None,
        Tree,
        Flat,
    };

    std::weak_ptr<SecureShell::SftpSession> sftp_;
    CommonOptions common_;
    std::unique_ptr<ScanPhaseDriver> scanPhase_{};
    EntrySource entrySource_{EntrySource::None};
    std::vector<SharedData::DirectoryEntry> entries_{};
    // Index aligned with entries_ in prescanned mode, empty in tree mode.
    std::vector<EntryPaths> prescannedPaths_{};
    std::vector<Slot> slots_{};
    // Entries whose child could not get a transfer buffer; retried once a slot frees up.
    std::deque<std::size_t> deferred_{};
    bool retryDeferred_{true};
    bool bufferShortageLogged_{false};
    std::size_t cursor_{0};
    std::uint64_t completedEntries_{0};
    std::uint64_t completedBytes_{0};
    std::uint64_t totalBytes_{0};
    int budget_{1};
    BulkResumeProgress earlierProgress_{};
    /** @brief Per entry: transferred, created or skipped for its type, so a resume leaves it out. */
    std::vector<bool> finishedEntries_{};
    std::vector<std::pair<std::string, Error>> failedEntries_{};
    std::vector<SharedData::IndexedFailure> indexedFailures_{};
    // Rolling bulk-level throughput. Per-file rates reset between files, which made the number
    // flicker; sampling the cumulative byte count keeps one coherent rate per bulk.
    std::chrono::steady_clock::time_point lastBpsSampleTime_{};
    std::uint64_t lastBpsSampleBytes_{0};
    std::int64_t bulkBytesPerSecond_{0};
    std::chrono::steady_clock::time_point lastEmit_{};
};

namespace Detail
{
    /**
     * @brief Both ends of the entry @p entryIndex of a scanned tree: its names on the way from the root joined below
     *        @p remoteRoot and @p localRoot. InvalidName when a name is no path segment or the local side cannot hold
     *        it, ImplementationError for a missing root or an index out of range.
     */
    std::expected<BulkTransferOperation::EntryPaths, Operation::Error> treeEntryPaths(
        std::vector<SharedData::DirectoryEntry> const& entries,
        std::size_t entryIndex,
        Paths::RemotePath const& remoteRoot,
        Paths::LocalPath const& localRoot
    );
}
