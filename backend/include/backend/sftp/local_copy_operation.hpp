#pragma once

#include <backend/sftp/operation.hpp>
#include <paths/local_path.hpp>
#include <shared_data/file_operations/local_copy_request.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/no_follow_tree_walk.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Copies or moves local items in the LocalOperationQueue, a work step at a time, so it can be held and
 *        canceled and shows progress. Links and junctions are copied as links and never followed.
 *
 * A move renames each item first and copies it only when that is not possible, across file systems or into an
 * existing directory, then removes the source unless something of it could not be copied. A copy onto its own source
 * duplicates the item next to itself under a free name. A file is written to a hidden sibling and renamed over its
 * destination once complete, so a failed copy never leaves a half written file in place of an existing one.
 */
class LocalCopyOperation : public Operation
{
  public:
    struct Progress
    {
        /** @brief The item handled last. */
        std::string currentItem{};
        std::uint64_t entriesDone{0};
        std::uint64_t entriesTotal{0};
        std::uint64_t bytesDone{0};
        std::uint64_t bytesTotal{0};
        /** @brief The file being copied, unset between files. */
        std::optional<std::string> currentFile{};
        std::uint64_t fileBytesDone{0};
        std::uint64_t fileBytesTotal{0};
    };

    struct LocalCopyOperationOptions
    {
        /** @brief Called at most every @ref progressInterval and once more when the copy ends. */
        std::function<void(Progress const&)> progress = [](Progress const&) {};
        /** @brief The bytes and entries counted before the copy starts, at most every @ref progressInterval. */
        std::function<void(std::uint64_t bytes, std::uint64_t entries)> scanProgress = [](auto, auto) {};
        std::vector<SharedData::LocalCopyEntry> entries{};
        bool move{false};
        /** @brief How long one work step runs before it hands back to the queue. */
        std::chrono::milliseconds stepBudget{20};
        std::chrono::milliseconds progressInterval{100};
        std::size_t chunkSize{1024u * 1024u};
    };

    /** @brief At most this many failed items are listed, the others are only counted and logged. */
    constexpr static std::size_t maximumListedFailures = 1000;

    /**
     * @brief The first name next to @p source that is free: "name (copy).ext", then "name (copy 2).ext" and on.
     *        Unset when none of the tried names is free or the check fails.
     */
    static std::optional<std::filesystem::path> freeCopyName(std::filesystem::path const& source);

    /**
     * @brief True when @p path is @p directory or lies below it, compared lexically.
     */
    static bool isSameOrBelow(std::filesystem::path const& path, std::filesystem::path const& directory);

    explicit LocalCopyOperation(LocalCopyOperationOptions options);
    ~LocalCopyOperation() override;
    LocalCopyOperation(LocalCopyOperation const&) = delete;
    LocalCopyOperation(LocalCopyOperation&&) = delete;
    LocalCopyOperation& operator=(LocalCopyOperation const&) = delete;
    LocalCopyOperation& operator=(LocalCopyOperation&&) = delete;

    SecureShell::ProcessingStrand* strand() const override
    {
        return nullptr;
    }

    bool usesStrand() const noexcept override
    {
        return false;
    }

    SharedData::OperationType type() const override
    {
        return options_.move ? SharedData::OperationType::LocalMove : SharedData::OperationType::LocalCopy;
    }

    bool isBarrier() const noexcept override
    {
        return false;
    }

    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    /**
     * @brief Works until the step budget is used. Completes when some items could not be copied, those are listed in
     *        @ref getFailed, and fails only when not a single entry arrived.
     */
    std::expected<WorkStatus, Error> work() override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;

    std::vector<std::pair<std::string, Error>> const& getFailed() const noexcept
    {
        return failedEntries_;
    }

  private:
    enum class Phase
    {
        Prepare,
        Scan,
        Copy,
        RemoveSource,
        Done
    };

    struct PreparedEntry
    {
        std::filesystem::path source{};
        std::filesystem::path destination{};
        Utility::NoFollowKind kind{Utility::NoFollowKind::Missing};
        bool mayOverwrite{false};
        /** @brief Renamed, failed or nothing to do: neither scanned nor copied. */
        bool settled{false};
        std::uint64_t entries{0};
        std::uint64_t bytes{0};
    };

    struct DirectoryFrame
    {
        std::filesystem::path source;
        std::filesystem::path destination;
        std::filesystem::directory_iterator iterator;
    };

    struct FileCopy
    {
        std::filesystem::path source;
        std::filesystem::path destination;
        std::filesystem::path partial;
        std::ifstream input;
        std::ofstream output;
        std::uint64_t copied{0};
        std::uint64_t total{0};
    };

    bool workPrepare(std::chrono::steady_clock::time_point deadline);
    bool workScan(std::chrono::steady_clock::time_point deadline);
    bool workCopy(std::chrono::steady_clock::time_point deadline);
    bool workRemoveSource(std::chrono::steady_clock::time_point deadline);

    void prepare(SharedData::LocalCopyEntry const& entry);
    void startEntryCopy();
    void finishEntryCopy();

    /**
     * @brief Copies the item @p source of @p kind to @p destination: a link as a link, a directory by entering it, a
     *        regular file by starting @ref file_.
     * @param replace An existing destination may be replaced or merged with.
     */
    void copyItem(
        std::filesystem::path const& source,
        std::filesystem::path const& destination,
        Utility::NoFollowKind kind,
        bool replace
    );
    void startFile(std::filesystem::path const& source, std::filesystem::path const& destination);
    bool continueFile(std::chrono::steady_clock::time_point deadline);
    void finishFile();
    void discardFile();
    void leaveDirectory();

    void recordFailure(std::filesystem::path const& path, Error error);
    void recordFailure(std::filesystem::path const& path, ErrorType type, std::error_code const& error);
    void reportProgress(bool final);

  private:
    LocalCopyOperationOptions options_;
    Phase phase_{Phase::Prepare};
    std::vector<PreparedEntry> prepared_{};
    std::size_t entryIndex_{0};
    std::vector<DirectoryFrame> directories_{};
    std::optional<FileCopy> file_{};
    std::vector<char> buffer_{};
    std::optional<Utility::NoFollowTreeWalk> walk_{};
    /** @brief Something of the current entry could not be copied, so a move keeps its source. */
    bool entryHadFailure_{false};
    bool walkingEntry_{false};
    std::vector<std::pair<std::string, Error>> failedEntries_{};
    std::size_t failureCount_{0};
    std::size_t arrivedEntries_{0};
    std::uint64_t countedEntries_{0};
    std::uint64_t countedBytes_{0};
    std::uint64_t entriesDone_{0};
    std::uint64_t bytesDone_{0};
    std::string lastItem_{};
    std::chrono::steady_clock::time_point lastProgress_{};
};
