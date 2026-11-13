#include <ftp/remote/ftp_connection.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::errorOf;
        using RemoteIo::RemoteError;
        using RemoteIo::RemoteErrorKind;

        template <typename ValueT>
        std::future<ValueT> readyFuture(ValueT value)
        {
            std::promise<ValueT> promise{};
            promise.set_value(std::move(value));
            return promise.get_future();
        }

        asio::awaitable<Engine::Result<LoginResult>> logInWith(std::shared_ptr<Pool> pool)
        {
            if (!pool)
                co_return std::unexpected(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session ended"));
            co_return co_await pool->start();
        }
    }

    struct FtpConnection::Shared
    {
        explicit Shared(LostHandler handler)
            : onLost{std::move(handler)}
        {}

        void lost(RemoteError const& error)
        {
            std::unique_lock lock{mutex};
            if (closing || reported)
                return;
            if (!started)
            {
                if (!pendingLoss)
                    pendingLoss = error;
                return;
            }
            reported = true;
            const auto handler = onLost;
            lock.unlock();
            if (handler)
                handler(error);
        }

        void start()
        {
            std::unique_lock lock{mutex};
            if (std::exchange(started, true) || closing || reported || !pendingLoss)
                return;
            reported = true;
            const auto error = *std::exchange(pendingLoss, std::nullopt);
            const auto handler = onLost;
            lock.unlock();
            if (handler)
                handler(error);
        }

        void close()
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }

        bool beginLogin()
        {
            std::scoped_lock lock{mutex};
            return !std::exchange(loginStarted, true);
        }

        void loggedIn(std::string directory)
        {
            std::scoped_lock lock{mutex};
            homeDirectory = std::move(directory);
        }

        std::optional<std::string> loggedInHome() const
        {
            std::scoped_lock lock{mutex};
            return homeDirectory;
        }

        mutable std::mutex mutex{};
        const LostHandler onLost;
        bool started{false};
        bool closing{false};
        bool reported{false};
        bool loginStarted{false};
        std::optional<RemoteError> pendingLoss{};
        /** @brief Set once the login succeeded, empty when the server told no directory. */
        std::optional<std::string> homeDirectory{};
    };

    FtpConnection::FtpConnection(
        asio::any_io_executor executor,
        std::shared_ptr<Resolver> resolver,
        FtpConnectionOptions options,
        LostHandler onLost
    )
        : options_{std::move(options)}
        , gate_{std::make_shared<TaskGate>(thread_)}
        , dialect_{std::make_shared<ServerDialect>()}
        , shared_{std::make_shared<Shared>(std::move(onLost))}
        , engine_{Engine::create(
              std::move(executor),
              std::move(resolver),
              gate_,
              EngineOptions{.pool = options_.pool},
              [shared = shared_](RemoteError const& error)
              {
                  shared->lost(error);
              }
          )}
    {
        thread_.start();
    }

    FtpConnection::~FtpConnection()
    {
        shared_->close();
        // The engine first: it fails the calls running now, so the final tasks of the file systems wait behind none.
        if (!engine_->close(options_.teardown))
            Log::warn("FTP: Work of the session ran on past its close");
        for (auto const& fileSystem : takeFileSystems())
            fileSystem->close();
        thread_.stop();
    }

    std::future<std::expected<void, RemoteError>> FtpConnection::logIn()
    {
        const auto promise = std::make_shared<std::promise<std::expected<void, RemoteError>>>();
        auto future = promise->get_future();
        if (!shared_->beginLogin())
        {
            promise->set_value(std::unexpected(errorOf(RemoteErrorKind::Failure, "The FTP session logs in only once")));
            return future;
        }
        engine_->run<LoginResult>(
            [weak = std::weak_ptr{engine_}](std::shared_ptr<CallScope> const&)
            {
                const auto engine = weak.lock();
                return logInWith(engine ? engine->pool() : nullptr);
            },
            options_.loginLimits,
            [shared = shared_, promise](Engine::Result<LoginResult> result)
            {
                if (!result)
                {
                    promise->set_value(std::unexpected(std::move(result).error()));
                    return;
                }
                shared->loggedIn(std::move(result->homeDirectory));
                promise->set_value({});
            }
        );
        return future;
    }

    void FtpConnection::start()
    {
        shared_->start();
    }

    std::future<std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteError>>
    FtpConnection::createFileSystem()
    {
        using Result = std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteError>;
        if (engine_->stopping() || engine_->failed() || !engine_->pool())
            return readyFuture<Result>(std::unexpected(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session ended")));
        auto homeDirectory = shared_->loggedInHome();
        if (!homeDirectory)
            return readyFuture<Result>(
                std::unexpected(errorOf(RemoteErrorKind::NoConnection, "The FTP session is not logged in"))
            );
        auto fileSystem = FtpFileSystem::create(
            FileSystemSession{
                .engine = engine_,
                .dialect = dialect_,
                .homeDirectory = std::move(*homeDirectory),
                .parallelTransfers = options_.pool.transferConnections,
            },
            thread_.createStrand(),
            options_.fileSystem
        );
        std::weak_ptr<RemoteIo::IRemoteFileSystem> handle = fileSystem;
        {
            std::scoped_lock lock{fileSystemsMutex_};
            std::erase_if(
                fileSystems_,
                [](auto const& open)
                {
                    return open->closed();
                }
            );
            fileSystems_.push_back(std::move(fileSystem));
        }
        return readyFuture<Result>(std::move(handle));
    }

    std::vector<std::shared_ptr<FtpFileSystem>> FtpConnection::takeFileSystems()
    {
        std::scoped_lock lock{fileSystemsMutex_};
        return std::exchange(fileSystems_, {});
    }
}
