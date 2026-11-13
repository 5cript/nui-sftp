#include <ftp/remote/running_operations.hpp>

namespace FileTransferProtocol
{
    std::optional<std::uint64_t> RunningOperations::add(std::weak_ptr<FailableOperation> operation)
    {
        std::scoped_lock lock{mutex_};
        if (closed_)
            return std::nullopt;
        const auto key = nextKey_++;
        running_.emplace(key, std::move(operation));
        return key;
    }

    void RunningOperations::remove(std::uint64_t key)
    {
        std::scoped_lock lock{mutex_};
        running_.erase(key);
    }

    bool RunningOperations::close(RemoteIo::RemoteError const& error)
    {
        std::unordered_map<std::uint64_t, std::weak_ptr<FailableOperation>> running{};
        {
            std::scoped_lock lock{mutex_};
            if (closed_)
                return false;
            closed_ = true;
            running = std::exchange(running_, {});
        }
        for (auto const& [key, weakOperation] : running)
        {
            if (const auto operation = weakOperation.lock())
                operation->fail(error);
        }
        return true;
    }

    bool RunningOperations::closed() const
    {
        std::scoped_lock lock{mutex_};
        return closed_;
    }

    std::size_t RunningOperations::size() const
    {
        std::scoped_lock lock{mutex_};
        return running_.size();
    }
}
