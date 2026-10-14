#pragma once

#include <backend/sftp/operation.hpp>
#include <paths/local_path.hpp>
#include <utility/no_follow_tree_walk.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

/**
 * @brief Deletes local items in the LocalOperationQueue, so they can be held and canceled and show progress like
 *        remote deletes. Each work step deletes until its time budget is used. Links and junctions are removed
 *        themselves and never followed. The queue checks the items with the local delete guard before it adds the
 *        operation.
 */
class LocalDeleteOperation : public Operation
{
  public:
    struct LocalDeleteOperationOptions
    {
        /**
         * @brief The item handled last, the entries handled so far and the total. Called at most every
         *        @ref progressInterval and once more when the deletion ends.
         */
        std::function<void(std::string const&, std::uint64_t current, std::uint64_t max)> itemsRemovedProgress =
            [](std::string const&, auto, auto) {};
        /**
         * @brief Recursive only: the bytes and entries counted before the deletion starts, at most every
         *        @ref progressInterval and once when the count is done.
         */
        std::function<void(std::uint64_t bytes, std::uint64_t entries)> scanProgress = [](auto, auto) {};
        /** @brief Deleted from the back, so without @ref recursive a directory goes before its content in this list. */
        std::vector<Paths::LocalPath> targets{};
        /**
         * @brief Every target goes with everything below it, counted first. Without, exactly the targets are removed,
         *        a directory only once it is empty.
         */
        bool recursive{false};
        /**
         * @brief Empty, or what a sync compare saw at each target, index aligned with @ref targets. A target that no
         *        longer holds it is not deleted and fails with ChangedSinceCompare.
         */
        std::vector<SharedData::ComparedState> comparedStates{};
        /** @brief How long one work step deletes before it hands back to the queue. */
        std::chrono::milliseconds stepBudget{20};
        std::chrono::milliseconds progressInterval{100};
    };

    /** @brief At most this many failed items are listed, the others are only counted and logged. */
    constexpr static std::size_t maximumListedFailures = 1000;

    /**
     * @brief The deepest directory that every one of @p targets lies below, the first target itself when it is a
     *        root. Unset when there are no targets or they share no directory.
     */
    static std::optional<Paths::LocalPath> commonParentOf(std::span<Paths::LocalPath const> targets);

    explicit LocalDeleteOperation(LocalDeleteOperationOptions options);
    ~LocalDeleteOperation() override = default;
    LocalDeleteOperation(LocalDeleteOperation const&) = delete;
    LocalDeleteOperation(LocalDeleteOperation&&) = delete;
    LocalDeleteOperation& operator=(LocalDeleteOperation const&) = delete;
    LocalDeleteOperation& operator=(LocalDeleteOperation&&) = delete;

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
        return SharedData::OperationType::LocalDelete;
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
     * @brief Deletes until the step budget is used. Completes as a partial success when some items could not be
     *        deleted, and fails only when not a single target could be deleted. An item that is already gone counts
     *        as deleted.
     */
    std::expected<WorkStatus, Error> work() override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;

    /**
     * @brief Items that could not be deleted, with why, at most @ref maximumListedFailures. The rest was still deleted.
     */
    std::vector<std::pair<std::string, Error>> const& getFailed() const noexcept
    {
        return failedEntries_;
    }

  private:
    bool workFlat(std::chrono::steady_clock::time_point deadline);
    bool workRecursive(std::chrono::steady_clock::time_point deadline);
    void finishWalk();
    void recordFailure(std::filesystem::path const& path, std::error_code const& error);
    void recordFailure(std::string const& path, Error error);
    void reportProgress(bool final);
    std::uint64_t handledEntries() const;

  private:
    LocalDeleteOperationOptions options_;
    std::vector<std::pair<std::string, Error>> failedEntries_{};
    std::size_t failureCount_{0};
    std::size_t targetIndex_{0};
    std::size_t completedTargets_{0};
    bool counting_{false};
    std::optional<Utility::NoFollowTreeWalk> walk_{};
    std::uint64_t countedEntries_{0};
    std::uint64_t countedBytes_{0};
    std::uint64_t removedEntries_{0};
    std::string lastItem_{};
    std::chrono::steady_clock::time_point lastProgress_{};
};
