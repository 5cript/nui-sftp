#include "queue_client.hpp"

#include <backend/sftp/kept_transfer_registry.hpp>
#include <constants/sftp.hpp>
#include <ids/ids.hpp>
#include <paths/local_path.hpp>
#include <paths/native.hpp>
#include <paths/remote_path.hpp>
#include <persistence/state/sftp_options.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>

#include <chrono>
#include <future>
#include <type_traits>
#include <utility>

namespace Benchmark
{
    namespace
    {
        constexpr auto transferTimeout = std::chrono::seconds{600};

        /**
         * @brief Runs @p function on @p strand and waits for it.
         */
        template <typename FunctionT>
        auto runOnStrand(boost::asio::strand<boost::asio::any_io_executor>& strand, FunctionT&& function)
        {
            std::packaged_task<std::invoke_result_t<FunctionT>()> task{std::forward<FunctionT>(function)};
            auto result = task.get_future();
            boost::asio::post(strand, std::move(task));
            return result.get();
        }
    }

    QueueClient::QueueClient(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem,
        std::string name,
        Protocol protocol,
        Nui::Window& window,
        Nui::RpcHub& hub,
        Queueing queueing
    )
        : fileSystem_{std::move(fileSystem)}
        , name_{std::move(name)}
        , protocol_{protocol}
        , queueing_{queueing}
        , strand_{std::make_shared<boost::asio::strand<boost::asio::any_io_executor>>(
              boost::asio::make_strand(pool_.get_executor())
          )}
    {
        const Persistence::SftpOptions options{};
        queue_ = std::make_shared<OperationQueue>(
            pool_.get_executor(),
            strand_,
            window,
            hub,
            options,
            Ids::generateSessionId(),
            std::make_shared<KeptTransferRegistry>(),
            options.concurrency.value_or(Persistence::defaultSftpConcurrency)
        );
        runOnStrand(
            *strand_,
            [this]()
            {
                queue_->setCompletionObserver(
                    [this](SharedData::OperationCompleted const& completed)
                    {
                        onCompleted(completed);
                    }
                );
                queue_->setWake(
                    [this]()
                    {
                        boost::asio::post(
                            *strand_,
                            [this]()
                            {
                                scheduleWork();
                            }
                        );
                    }
                );
                queue_->paused(false);
            }
        );
    }

    QueueClient::~QueueClient()
    {
        runOnStrand(
            *strand_,
            [this]()
            {
                queue_->paused(true);
                queue_.reset();
            }
        );
        pool_.join();
    }

    std::string QueueClient::name() const
    {
        return name_;
    }

    Protocol QueueClient::protocol() const
    {
        return protocol_;
    }

    bool QueueClient::supports(Workload const& workload) const
    {
        // One file per operation is what the other queue client does for a single file already.
        return queueing_ == Queueing::AsTheApp || !workload.isSingleFile();
    }

    std::expected<void, std::string> QueueClient::transfer(Transfer const& transfer)
    {
        const auto items = itemsOf(transfer);
        std::future<void> allCompleted;
        {
            std::scoped_lock lock{mutex_};
            expected_.emplace(Expected{.count = items.size()});
            allCompleted = expected_->allCompleted.get_future();
        }

        const auto added = runOnStrand(
            *strand_,
            [&]() -> std::expected<void, std::string>
            {
                for (auto const& item : items)
                {
                    if (auto result = add(transfer, item); !result)
                        return result;
                }
                scheduleWork();
                return {};
            }
        );
        if (!added)
            return added;

        if (allCompleted.wait_for(transferTimeout) != std::future_status::ready)
            return std::unexpected{fmt::format("did not complete within {} s", transferTimeout.count())};

        std::scoped_lock lock{mutex_};
        const auto failures = std::move(expected_->failures);
        expected_.reset();
        if (!failures.empty())
            return std::unexpected{
                fmt::format("{} of {} failed, first: {}", failures.size(), items.size(), failures.front())
            };
        return {};
    }

    std::vector<QueueClient::Item> QueueClient::itemsOf(Transfer const& transfer) const
    {
        if (queueing_ == Queueing::AsTheApp || transfer.workload->isSingleFile())
            return {Item{.localPath = transfer.localPath, .remotePath = transfer.remotePath}};

        std::vector<Item> items;
        items.reserve(transfer.files.size());
        for (auto const& file : transfer.files)
        {
            items.push_back(
                Item{
                    .localPath = transfer.localPath / Paths::nativeFromUtf8(file),
                    .remotePath = fmt::format("{}/{}", transfer.remotePath, file),
                }
            );
        }
        return items;
    }

    std::expected<void, std::string> QueueClient::add(Transfer const& transfer, Item const& item)
    {
        const auto local = Paths::LocalPath::make(item.localPath);
        const auto remote = Paths::RemotePath::make(item.remotePath);
        if (!local)
            return std::unexpected{fmt::format("invalid local path {}", Paths::nativeToUtf8(item.localPath))};
        if (!remote)
            return std::unexpected{fmt::format("invalid remote path {}", item.remotePath)};

        const auto isBigFile = transfer.workload->fileSize >= Constants::bigFileCutOff;
        const auto operationId = Ids::generateOperationId();
        const auto added = transfer.direction == Direction::Download
            ? queue_->addDownloadOperation(fileSystem_, operationId, *local, *remote, false, isBigFile, false, true)
            : queue_->addUploadOperation(fileSystem_, operationId, *local, *remote, false, isBigFile, false, true);
        if (!added)
            return std::unexpected{fmt::format("adding {} failed: {}", item.remotePath, added.error().toString())};
        return {};
    }

    void QueueClient::onCompleted(SharedData::OperationCompleted const& completed)
    {
        std::scoped_lock lock{mutex_};
        if (!expected_)
            return;
        if (completed.reason != SharedData::OperationCompletionReason::Completed)
            expected_->failures.push_back(
                completed.error ? completed.error->toString()
                                : fmt::format("ended as {}", static_cast<int>(completed.reason))
            );
        else if (!completed.failedEntries.empty())
            expected_->failures.push_back(
                fmt::format(
                    "{}: {}", completed.failedEntries.front().first, completed.failedEntries.front().second.toString()
                )
            );
        if (++expected_->completed == expected_->count)
            expected_->allCompleted.set_value();
    }

    void QueueClient::scheduleWork()
    {
        if (std::exchange(workScheduled_, true))
            return;
        boost::asio::post(
            *strand_,
            [this]()
            {
                doWork();
            }
        );
    }

    void QueueClient::doWork()
    {
        workScheduled_ = false;
        if (!queue_)
            return;
        // As Session::doOperationQueueWork: posted, not looped, a waiting or empty queue wakes once it has work.
        if ((!queue_->paused() || queue_->hasPriorityWork()) && queue_->work())
            scheduleWork();
    }
}
