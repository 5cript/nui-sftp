#include <ftp/support/file_system_harness.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/engine/task_gate.hpp>
#include <ftp/remote/ftp_file_system.hpp>
#include <remote_io/async/processing_thread.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <fmt/format.h>

#include <chrono>
#include <future>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace asio = boost::asio;
    using namespace std::chrono_literals;

    class FileSystemHarness::Implementation
    {
      public:
        Implementation()
        {
            thread_.start();
        }

        ~Implementation()
        {
            if (fileSystem_)
                fileSystem_->close();
            fileSystem_.reset();
            if (engine_)
                engine_->close();
            gate_->close();
            thread_.stop();
            engine_.reset();
        }

        Implementation(Implementation const&) = delete;
        Implementation& operator=(Implementation const&) = delete;
        Implementation(Implementation&&) = delete;
        Implementation& operator=(Implementation&&) = delete;

        std::expected<void, std::string> connect(Options const& options)
        {
            EngineOptions engineOptions{};
            engineOptions.pool = PoolOptions{
                .host = options.host,
                .port = options.port,
                .login = {.credentials = {.user = "user", .password = "password"}},
                .control = {.connectTimeout = 10s, .commandTimeout = 10s},
                .transferConnections = options.transferConnections,
            };
            engine_ = Engine::create(
                executor_.get_executor(), std::make_shared<Resolver>(), gate_, std::move(engineOptions)
            );
            auto started = asio::co_spawn(engine_->strand(), engine_->pool()->start(), asio::use_future);
            if (started.wait_for(20s) != std::future_status::ready)
                return std::unexpected(std::string{"The login did not end"});
            const auto login = started.get();
            if (!login)
                return std::unexpected(fmt::format("The login failed: {}", login.error().toString()));
            fileSystem_ = FtpFileSystem::create(
                FileSystemSession{
                    .engine = engine_,
                    .homeDirectory = login->homeDirectory,
                    .parallelTransfers = options.transferConnections,
                },
                thread_.createStrand()
            );
            return {};
        }

        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem() const
        {
            return fileSystem_;
        }

      private:
        asio::thread_pool executor_{2u};
        RemoteIo::ProcessingThread thread_{};
        std::shared_ptr<TaskGate> gate_ = std::make_shared<TaskGate>(thread_);
        std::shared_ptr<Engine> engine_{};
        std::shared_ptr<FtpFileSystem> fileSystem_{};
    };

    FileSystemHarness::FileSystemHarness(std::unique_ptr<Implementation> implementation)
        : impl_{std::move(implementation)}
    {}

    FileSystemHarness::~FileSystemHarness() = default;

    std::expected<std::unique_ptr<FileSystemHarness>, std::string>
    FileSystemHarness::connect(Options const& options)
    {
        auto implementation = std::make_unique<Implementation>();
        if (auto connected = implementation->connect(options); !connected)
            return std::unexpected(std::move(connected).error());
        return std::unique_ptr<FileSystemHarness>{new FileSystemHarness{std::move(implementation)}};
    }

    std::shared_ptr<RemoteIo::IRemoteFileSystem> FileSystemHarness::fileSystem() const
    {
        return impl_->fileSystem();
    }
}
