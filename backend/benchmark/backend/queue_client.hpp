#pragma once

#include "client.hpp"

#include <backend/sftp/operation_queue.hpp>
#include <nui/rpc.hpp>
#include <nui/window.hpp>
#include <remote_io/remote_file_system.hpp>
#include <shared_data/file_operations/operation_completed.hpp>

#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Benchmark
{
    /**
     * @brief How a queue client hands a workload to its queue.
     */
    enum class Queueing
    {
        /** @brief As the app does: one operation for the file or the directory, a directory as a bulk transfer. */
        AsTheApp,
        /** @brief Every file as an operation of its own, which the queue runs side by side. */
        OnePerFile,
    };

    /**
     * @brief Transfers through an OperationQueue with the default options, worked the way Session works it: a wake
     *        posts work to the strand, which works again right away while the queue has work. Times from the first
     *        add until every operation completed.
     */
    class QueueClient : public Client
    {
      public:
        QueueClient(
            std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem,
            std::string name,
            Protocol protocol,
            Nui::Window& window,
            Nui::RpcHub& hub,
            Queueing queueing
        );
        ~QueueClient() override;
        QueueClient(QueueClient const&) = delete;
        QueueClient& operator=(QueueClient const&) = delete;
        QueueClient(QueueClient&&) = delete;
        QueueClient& operator=(QueueClient&&) = delete;

        std::string name() const override;
        Protocol protocol() const override;
        bool supports(Workload const& workload) const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;

      private:
        /**
         * @brief One operation the queue gets: a file or the root of a workload.
         */
        struct Item
        {
            std::filesystem::path localPath{};
            std::string remotePath{};
        };

        struct Expected
        {
            std::size_t count{0};
            std::size_t completed{0};
            std::vector<std::string> failures{};
            std::promise<void> allCompleted{};
        };

        std::vector<Item> itemsOf(Transfer const& transfer) const;
        std::expected<void, std::string> add(Transfer const& transfer, Item const& item);
        void onCompleted(SharedData::OperationCompleted const& completed);
        void scheduleWork();
        void doWork();

      private:
        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem_;
        std::string name_;
        Protocol protocol_;
        Queueing queueing_;
        boost::asio::thread_pool pool_{4};
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_;
        std::shared_ptr<OperationQueue> queue_;
        bool workScheduled_{false};
        std::mutex mutex_{};
        std::optional<Expected> expected_{};
    };
}
