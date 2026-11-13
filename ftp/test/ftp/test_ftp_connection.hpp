#pragma once

#include "test_engine.hpp"
#include "test_metadata.hpp"

#include <ftp/remote/ftp_connection.hpp>
#include <ftp/support/tree_responder.hpp>
#include <paths/remote_path.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;

        /**
         * @brief Serves a tree, but answers a line that @p rule picks as the rule says.
         */
        class TroubledTreeResponder : public Responder
        {
          public:
            using Rule = std::function<std::optional<Script>(std::string_view line)>;

            TroubledTreeResponder(std::shared_ptr<ServedTree> tree, Rule rule)
                : tree_{std::move(tree)}
                , rule_{std::move(rule)}
            {}

            Script answer(std::string_view line) override
            {
                if (auto special = rule_(line))
                    return std::move(*special);
                return tree_.answer(line);
            }

          private:
            TreeResponder tree_;
            Rule rule_;
        };

        Script servingWith(std::shared_ptr<ServedTree> tree, TroubledTreeResponder::Rule rule)
        {
            Script script{};
            script.reply("220 Served").serve(std::make_shared<TroubledTreeResponder>(std::move(tree), std::move(rule)));
            return script;
        }

        /**
         * @brief A listing of /slow never ends, a download of /slow/big stops after its first bytes.
         */
        std::optional<Script> slowCorner(std::string_view line)
        {
            if (line == "CWD /slow")
            {
                Script script{};
                script.checkpoint("listing stalled").stall();
                return script;
            }
            if (line == "RETR /slow/big")
            {
                Script script{};
                script.reply("150 Opening BINARY mode data connection.")
                    .beginData(DataPlan{}.stallAfter(std::string(4096u, 'x')));
                return script;
            }
            return std::nullopt;
        }

        /**
         * @brief C0 breaks off at the first listing.
         */
        std::optional<Script> resetsAtTheFirstListing(std::string_view line)
        {
            if (!line.starts_with("CWD"))
                return std::nullopt;
            Script script{};
            script.resetControl();
            return script;
        }

        std::vector<ServedEntry> servedRoot()
        {
            return {
                servedDirectory("dir", {servedFile("a.txt", "alpha"), servedFile("b.txt", "beta")}),
                servedDirectory("slow", {servedFile("big", std::string(65536u, 'x'))}),
                servedDirectory("data", {servedFile("big", std::string(256u * 1024u, 'd'))}),
            };
        }

        Paths::RemotePath connectionPath(std::string_view path)
        {
            return Paths::RemotePath::make(path).value();
        }

        Script refusedLogin()
        {
            Script refused{};
            refused.reply("220 Ready")
                .expect("USER", "user")
                .reply("331 Password required")
                .expect("PASS", "password")
                .reply("530 Login incorrect.")
                .expectDisconnect();
            return refused;
        }

        using StreamPointer = std::shared_ptr<RemoteIo::IRemoteFileStream>;

        /**
         * @brief Reads one piece and closes the stream in the middle of its transfer, on the processing thread.
         */
        std::expected<void, RemoteIo::RemoteError> readOnceAndClose(StreamPointer const& stream)
        {
            std::vector<char> buffer(4096u);
            const auto read = stream->readSomeInStrand(buffer.data(), buffer.size());
            stream->closeInStrand();
            if (!read)
                return std::unexpected(read.error());
            return {};
        }

        /**
         * @brief Starts an asynchronous read of the whole file and closes the stream at once, on the processing
         *        thread. The buffer lives as long as the transfer's reader.
         */
        std::expected<void, RemoteIo::RemoteError> startReadingAndClose(StreamPointer const& stream)
        {
            using Size = RemoteIo::IRemoteFileStream::SignedSizeType;
            const auto buffer = std::make_shared<std::vector<char>>(64u * 1024u);
            const auto started = stream->readAsyncInStrand(
                static_cast<Size>(256u * 1024u),
                buffer->data(),
                static_cast<Size>(buffer->size()),
                [buffer](Size)
                {
                    return true;
                }
            );
            stream->closeInStrand();
            if (!started)
                return std::unexpected(started.error());
            return {};
        }

        /**
         * @brief The losses a handler was called with and the thread it ran on.
         */
        struct RecordedLosses
        {
            void record(RemoteIo::RemoteError const& error)
            {
                std::scoped_lock lock{mutex};
                errors.push_back(error);
                threads.push_back(std::this_thread::get_id());
                if (errors.size() == 1u)
                    first.set_value();
            }

            std::vector<RemoteIo::RemoteError> recorded() const
            {
                std::scoped_lock lock{mutex};
                return errors;
            }

            mutable std::mutex mutex{};
            std::vector<RemoteIo::RemoteError> errors{};
            std::vector<std::thread::id> threads{};
            std::promise<void> first{};
        };
    }

    class FtpConnectionTests : public ::testing::TestWithParam<EngineExecutor>
    {
      protected:
        using FileSystemPointer = std::shared_ptr<RemoteIo::IRemoteFileSystem>;

        void TearDown() override
        {
            connection_.reset();
            if (!server_)
                return;
            EXPECT_TRUE(server_->waitUntilDisconnected());
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
            if (!HasFailure())
                return;
            for (std::size_t connection = 0u; connection < addedConnections_; ++connection)
            {
                std::string commands{};
                for (auto const& command : server_->commands(connection))
                    commands += fmt::format("\n  {}", command);
                std::cout << fmt::format("Commands of connection {}:{}\n", connection, commands);
            }
        }

        /**
         * @brief Starts a server that serves the test tree on @p connections control connections.
         */
        void serve(std::size_t connections = 4u, TroubledTreeResponder::Rule rule = slowCorner)
        {
            tree_ = std::make_shared<ServedTree>(servedRoot());
            startServer();
            for (std::size_t connection = 0u; connection < connections; ++connection)
                addConnection(servingWith(tree_, rule));
        }

        void addConnection(Script script)
        {
            server_->addConnection(std::move(script));
            ++addedConnections_;
        }

        void startServer()
        {
            auto started = ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
        }

        FtpConnectionOptions options(std::size_t transferConnections = 2u) const
        {
            FtpConnectionOptions options{};
            options.pool = PoolOptions{
                .host = server_->address(),
                .port = server_->port(),
                .login = {.credentials = {.user = "user", .password = "password"}},
                .control = {.connectTimeout = 10s, .commandTimeout = 10s},
                .transferConnections = transferConnections,
                .reloginAttempts = 1u,
                .reloginBackoff = 10ms,
            };
            // Far beyond what the tests wait: an end before them is the close's doing.
            options.fileSystem.operationLimits = {.backstop = 60s, .drain = 1s};
            options.fileSystem.transferCallLimits = {.backstop = 90s, .drain = 1s};
            options.fileSystem.transfer.data.inactivityTimeout = 60s;
            options.teardown = {.quitBackstop = 1s, .drain = 500ms, .closeGrace = 1s};
            return options;
        }

        void connect(FtpConnectionOptions connectionOptions, FtpConnection::LostHandler onLost = {})
        {
            connection_ = std::make_unique<FtpConnection>(
                host_.executor(), resolver_, std::move(connectionOptions), std::move(onLost)
            );
        }

        void connectAndLogIn(std::size_t transferConnections = 2u, FtpConnection::LostHandler onLost = {})
        {
            connect(options(transferConnections), std::move(onLost));
            auto login = connection_->logIn();
            const auto loggedIn = valueOf(login);
            ASSERT_TRUE(loggedIn) << loggedIn.error().toString();
        }

        FileSystemPointer openFileSystem()
        {
            auto created = connection_->createFileSystem();
            const auto fileSystem = valueOf(created);
            if (!fileSystem)
                throw std::runtime_error{fileSystem.error().toString()};
            return fileSystem->lock();
        }

        /**
         * @brief One open of @p path for reading, which waits for the wake of a Busy one before it returns it.
         */
        RemoteIo::OperationResult<RemoteIo::OpenFile> openOnce(FileSystemPointer const& fileSystem, std::string_view path)
        {
            const auto free = std::make_shared<std::promise<void>>();
            auto woken = free->get_future();
            auto attempt = fileSystem->later(RemoteIo::OpenFile{
                .path = connectionPath(path),
                .mode = RemoteIo::OpenMode::Read,
                .wakeWhenFree =
                    [free]()
                    {
                        free->set_value();
                    },
            });
            auto opened = valueOf(attempt);
            if (!opened && opened.error().kind == RemoteErrorKind::Busy)
                EXPECT_EQ(woken.wait_for(10s), std::future_status::ready) << "a Busy open was never woken";
            return opened;
        }

        static std::future<RemoteIo::OperationResult<RemoteIo::ListDirectory>>
        listLater(FileSystemPointer const& fileSystem, std::string_view path)
        {
            return fileSystem->later(RemoteIo::ListDirectory{.path = connectionPath(path), .resolveLinks = false});
        }

        RemoteIo::OperationResult<RemoteIo::ListDirectory> list(FileSystemPointer const& fileSystem, std::string_view path)
        {
            auto listed = listLater(fileSystem, path);
            return valueOf(listed);
        }

        /**
         * @brief Opens @p path for reading once a transfer connection is free: a stream closed in the middle of its
         *        transfer gives its connection back after the server answered the ABOR. Waits for the wake of every
         *        Busy, at most 10 attempts.
         */
        StreamPointer openForReading(FileSystemPointer const& fileSystem, std::string_view path)
        {
            auto opened = openOnce(fileSystem, path);
            for (auto attempt = 1; attempt < 10 && !opened && opened.error().kind == RemoteErrorKind::Busy; ++attempt)
                opened = openOnce(fileSystem, path);
            if (!opened)
                throw std::runtime_error{opened.error().toString()};
            return opened->lock();
        }

        /**
         * @brief The thread that runs the tasks of @p fileSystem, a round trip there: tasks queued before have run.
         */
        static std::thread::id processingThreadOf(FileSystemPointer const& fileSystem)
        {
            return fileSystem
                ->performPromise(
                    []()
                    {
                        return std::this_thread::get_id();
                    },
                    []()
                    {
                        return std::thread::id{};
                    }
                )
                .get();
        }

        /**
         * @brief How long destroying the connection took.
         */
        std::chrono::steady_clock::duration closeConnection()
        {
            const auto start = std::chrono::steady_clock::now();
            connection_.reset();
            return std::chrono::steady_clock::now() - start;
        }

        ExecutorHost host_{GetParam()};
        std::shared_ptr<Resolver> resolver_ = std::make_shared<Resolver>();
        std::shared_ptr<ServedTree> tree_{};
        std::unique_ptr<ScriptedFtpServer> server_{};
        std::unique_ptr<FtpConnection> connection_{};
        std::size_t addedConnections_{0u};
    };

    TEST_P(FtpConnectionTests, FileSystemsOfOneConnectionShareItsSessionAndCloseWithIt)
    {
        serve();
        connectAndLogIn();
        connection_->start();
        const auto first = openFileSystem();
        const auto second = openFileSystem();
        const auto inFirst = list(first, "/dir");
        ASSERT_TRUE(inFirst) << inFirst.error().toString();
        EXPECT_EQ(inFirst->size(), 2u);
        const auto inSecond = list(second, "/dir");
        ASSERT_TRUE(inSecond) << inSecond.error().toString();
        EXPECT_EQ(processingThreadOf(first), processingThreadOf(second));

        closeConnection();
        EXPECT_FALSE(first->close()) << "the connection left a file system open";
        EXPECT_FALSE(second->close()) << "the connection left a file system open";
        const auto afterwards = list(first, "/dir");
        ASSERT_FALSE(afterwards);
        EXPECT_EQ(afterwards.error().kind, RemoteErrorKind::ConnectionLost) << afterwards.error().toString();
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->commands(0u).back(), "QUIT");
    }

    TEST_P(FtpConnectionTests, AFileSystemNeedsTheLoginWhichRunsOnce)
    {
        serve();
        connect(options());
        auto early = connection_->createFileSystem();
        const auto beforeLogin = valueOf(early);
        ASSERT_FALSE(beforeLogin);
        EXPECT_EQ(beforeLogin.error().kind, RemoteErrorKind::NoConnection) << beforeLogin.error().toString();

        auto login = connection_->logIn();
        const auto loggedIn = valueOf(login);
        ASSERT_TRUE(loggedIn) << loggedIn.error().toString();
        auto again = connection_->logIn();
        const auto second = valueOf(again);
        ASSERT_FALSE(second);
        EXPECT_EQ(second.error().kind, RemoteErrorKind::Failure) << second.error().toString();
        EXPECT_TRUE(list(openFileSystem(), "/dir"));
    }

    TEST_P(FtpConnectionTests, ARefusedLoginOpensNoFileSystem)
    {
        startServer();
        addConnection(refusedLogin());
        connect(options());
        auto login = connection_->logIn();
        const auto loggedIn = valueOf(login);
        ASSERT_FALSE(loggedIn);
        auto created = connection_->createFileSystem();
        const auto fileSystem = valueOf(created);
        ASSERT_FALSE(fileSystem);
        EXPECT_EQ(fileSystem.error().kind, RemoteErrorKind::NoConnection) << fileSystem.error().toString();
    }

    TEST_P(FtpConnectionTests, ALossAfterTheStartRunsTheHandlerOnceOnTheProcessingThread)
    {
        tree_ = std::make_shared<ServedTree>(servedRoot());
        startServer();
        addConnection(servingWith(tree_, resetsAtTheFirstListing));
        addConnection(refusedLogin());
        addConnection(refusedLogin());
        const auto losses = std::make_shared<RecordedLosses>();
        connectAndLogIn(
            1u,
            [losses](RemoteIo::RemoteError const& error)
            {
                losses->record(error);
            }
        );
        connection_->start();
        const auto fileSystem = openFileSystem();
        EXPECT_FALSE(list(fileSystem, "/dir"));

        ASSERT_EQ(losses->first.get_future().wait_for(10s), std::future_status::ready);
        const auto lost = list(fileSystem, "/dir");
        ASSERT_FALSE(lost);
        EXPECT_EQ(lost.error().kind, RemoteErrorKind::ConnectionLost) << lost.error().toString();
        const auto processingThread = processingThreadOf(fileSystem);
        ASSERT_EQ(losses->recorded().size(), 1u);
        std::scoped_lock lock{losses->mutex};
        EXPECT_EQ(losses->threads.front(), processingThread);
    }

    TEST_P(FtpConnectionTests, ALossBeforeTheStartIsReportedByTheStart)
    {
        tree_ = std::make_shared<ServedTree>(servedRoot());
        startServer();
        addConnection(servingWith(tree_, resetsAtTheFirstListing));
        addConnection(refusedLogin());
        addConnection(refusedLogin());
        const auto losses = std::make_shared<RecordedLosses>();
        connectAndLogIn(
            1u,
            [losses](RemoteIo::RemoteError const& error)
            {
                losses->record(error);
            }
        );
        const auto fileSystem = openFileSystem();
        EXPECT_FALSE(list(fileSystem, "/dir"));
        // A Lost session refuses at once and has queued its report by then, the round trip lets that report run.
        const auto lost = list(fileSystem, "/dir");
        ASSERT_FALSE(lost);
        ASSERT_EQ(lost.error().kind, RemoteErrorKind::ConnectionLost) << lost.error().toString();
        std::ignore = processingThreadOf(fileSystem);
        EXPECT_TRUE(losses->recorded().empty()) << "reported before the start";

        connection_->start();
        ASSERT_EQ(losses->recorded().size(), 1u);
        std::scoped_lock lock{losses->mutex};
        EXPECT_EQ(losses->threads.front(), std::this_thread::get_id());
    }

    TEST_P(FtpConnectionTests, BrowseOperationsStillWaitingFailWhenTheConnectionCloses)
    {
        serve();
        connectAndLogIn();
        connection_->start();
        const auto first = openFileSystem();
        const auto second = openFileSystem();
        std::vector<std::future<RemoteIo::OperationResult<RemoteIo::ListDirectory>>> waiting{};
        for (auto index = 0; index < 4; ++index)
        {
            waiting.push_back(listLater(first, "/slow"));
            waiting.push_back(listLater(second, "/slow"));
        }
        ASSERT_TRUE(server_->waitForCheckpoint("listing stalled"));

        const auto took = closeConnection();
        // The operations' backstop is 60 s: a close that waited for one would take that long.
        EXPECT_LT(took, 30s);
        for (auto& future : waiting)
        {
            ASSERT_EQ(future.wait_for(0s), std::future_status::ready) << "a browse operation outlived the close";
            const auto result = future.get();
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().kind, RemoteErrorKind::ConnectionLost) << result.error().toString();
        }
    }

    TEST_P(FtpConnectionTests, ACloseWhileTheLookupHangsFailsTheLogin)
    {
        startServer();
        auto release = std::make_shared<std::promise<void>>();
        auto released = release->get_future().share();
        const auto entered = std::make_shared<std::promise<void>>();
        resolver_ = std::make_shared<Resolver>(
            ResolverOptions{},
            [released, entered](std::string const& host, std::uint16_t port) -> ResolveResult
            {
                entered->set_value();
                released.wait();
                return Resolver::systemLookup(host, port);
            }
        );
        connect(options());
        auto login = connection_->logIn();
        ASSERT_EQ(entered->get_future().wait_for(10s), std::future_status::ready);

        const auto took = closeConnection();
        EXPECT_LT(took, 30s);
        ASSERT_EQ(login.wait_for(0s), std::future_status::ready) << "the login outlived the close";
        EXPECT_FALSE(login.get());
        release->set_value();
        resolver_.reset();
    }

    TEST_P(FtpConnectionTests, StreamsClosedInTheMiddleOfTheirTransfersLeaveTheSessionHealthy)
    {
        serve(8u);
        connectAndLogIn(3u);
        connection_->start();
        std::vector<FileSystemPointer> fileSystems{openFileSystem(), openFileSystem(), openFileSystem()};
        for (auto round = 0; round < 4; ++round)
        {
            // All streams first, so that their transfers and closes overlap on the connections.
            std::vector<StreamPointer> streams{};
            for (auto const& fileSystem : fileSystems)
                streams.push_back(openForReading(fileSystem, "/data/big"));
            const auto asynchronous = (round % 2) == 1;
            std::vector<std::future<std::expected<void, RemoteIo::RemoteError>>> transfers{};
            std::vector<std::future<RemoteIo::OperationResult<RemoteIo::ListDirectory>>> listings{};
            for (std::size_t index = 0u; index < fileSystems.size(); ++index)
            {
                transfers.push_back(fileSystems[index]->performPromise(
                    [stream = streams[index], asynchronous]() -> std::expected<void, RemoteIo::RemoteError>
                    {
                        return asynchronous ? startReadingAndClose(stream) : readOnceAndClose(stream);
                    }
                ));
                listings.push_back(listLater(fileSystems[index], "/dir"));
            }
            for (auto& transfer : transfers)
            {
                const auto ended = valueOf(transfer);
                EXPECT_TRUE(ended) << "round " << round << ": " << ended.error().toString();
            }
            for (auto& listing : listings)
            {
                const auto listed = valueOf(listing);
                EXPECT_TRUE(listed) << "round " << round << ": " << listed.error().toString();
            }
        }
        for (auto const& fileSystem : fileSystems)
        {
            const auto stream = openForReading(fileSystem, "/dir/a.txt");
            const auto content = std::make_shared<std::string>();
            auto read = stream->readAll(
                [content](std::string_view data)
                {
                    content->append(data);
                    return true;
                }
            );
            const auto bytes = valueOf(read);
            stream->close();
            ASSERT_TRUE(bytes) << bytes.error().toString();
            EXPECT_EQ(*content, "alpha");
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        FtpConnectionTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );
}
