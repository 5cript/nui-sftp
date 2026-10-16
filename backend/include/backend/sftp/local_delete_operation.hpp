#pragma once

#include <backend/sftp/operation.hpp>
#include <paths/local_path.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief Deletes local items through the queue, so they honour pause and show progress like remote deletes.
 *        Exactly the given items are removed: a directory only once empty, a link as the link itself.
 */
class LocalDeleteOperation : public Operation
{
  public:
    struct LocalDeleteOperationOptions
    {
        std::function<void(std::string const&, std::uint64_t current, std::uint64_t max)> itemsRemovedProgress =
            [](std::string const&, auto, auto) {};
        /** @brief Deleted from the back, so a directory goes before its content in this list. */
        std::vector<Paths::LocalPath> targets{};
    };

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

    std::expected<WorkStatus, Error> work() override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;

    /**
     * @brief Items that could not be deleted, with why. The rest was still deleted.
     */
    std::vector<std::pair<std::string, Error>> const& getFailed() const noexcept
    {
        return failedEntries_;
    }

  private:
    LocalDeleteOperationOptions options_;
    std::vector<std::pair<std::string, Error>> failedEntries_{};
    std::size_t deletedThusFar_{0};
};
