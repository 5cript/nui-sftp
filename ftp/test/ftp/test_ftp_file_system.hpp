#pragma once

#include "test_engine.hpp"
#include "test_metadata.hpp"

#include <ftp/remote/ftp_file_system.hpp>
#include <ftp/remote/running_operations.hpp>
#include <paths/remote_path.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        constexpr std::string_view listedDirectory = "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 b\r\n";
        constexpr std::string_view listedLinkToDirectory =
            "lrwxrwxrwx    1 0        0               3 Jan 02 03:04 up -> sub\r\n";
        constexpr std::string_view listedDanglingLink =
            "lrwxrwxrwx    1 0        0               4 Jan 02 03:04 gone -> none\r\n";
        constexpr std::string_view listedSubdirectory =
            "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 sub\r\n";

        Paths::RemotePath remote(std::string_view path)
        {
            return Paths::RemotePath::make(path).value();
        }

        std::vector<std::string> namesOf(std::vector<RemoteIo::FileInformation> const& entries)
        {
            std::vector<std::string> names{};
            for (auto const& entry : entries)
                names.push_back(entry.name);
            return names;
        }

        /**
         * @brief The listing of @p directory with LIST -a, which the tests' dialect knows to work.
         */
        Script& listsIn(Script& script, std::string directory, std::string data)
        {
            return listingOf(enters(script, std::move(directory)), "LIST", "-a", std::move(data));
        }

        /**
         * @brief Records the completions of later(): how many came and the first.
         */
        template <typename ResultT>
        struct Completions
        {
            void record(ResultT result)
            {
                std::scoped_lock lock{mutex};
                ++count;
                if (!first)
                {
                    first.emplace(std::move(result));
                    arrived.set_value();
                }
            }

            std::mutex mutex{};
            int count{0};
            std::optional<ResultT> first{};
            std::promise<void> arrived{};
        };
    }

    class FtpFileSystemTests : public ::testing::TestWithParam<EngineExecutor>
    {
      protected:
        void SetUp() override
        {
            thread_.start();
            dialect_->learnListAll(Support::Works);
            dialect_->learnStatus(Support::Refused);
        }

        void TearDown() override
        {
            fileSystem_.reset();
            if (engine_)
            {
                EXPECT_FALSE(engine_->failed()) << "an operation failed the session";
                engine_->refusePosts();
                EXPECT_TRUE(engine_->waitIdle(5s)) << "work of the session was still in flight";
            }
            gate_->close();
            thread_.stop();
            engine_.reset();
            if (server_)
            {
                EXPECT_TRUE(server_->waitUntilDisconnected());
                for (auto const& failure : server_->failures())
                    ADD_FAILURE() << failure;
            }
        }

        /**
         * @brief Starts a server whose C0 follows @p script after the login, logs in and makes a file system there.
         */
        std::shared_ptr<FtpFileSystem>
        connect(Script script, FtpFileSystemOptions options = {}, std::size_t transferConnections = 3u)
        {
            auto started = ScriptedFtpServer::start();
            if (!started)
                throw std::runtime_error{started.error()};
            server_ = std::move(started).value();
            server_->addConnection(std::move(script));
            EngineOptions engineOptions{};
            engineOptions.pool = PoolOptions{
                .host = server_->address(),
                .port = server_->port(),
                .login = {.credentials = {.user = "user", .password = "password"}},
                .control = {.connectTimeout = 10s, .commandTimeout = 10s},
                .transferConnections = transferConnections,
            };
            engine_ = Engine::create(host_.executor(), std::make_shared<Resolver>(), gate_, std::move(engineOptions));
            auto start = asio::co_spawn(engine_->strand(), engine_->pool()->start(), asio::use_future);
            const auto login = valueOf(start);
            if (!login)
                throw std::runtime_error{fmt::format("The login failed: {}", login.error().toString())};
            fileSystem_ = FtpFileSystem::create(
                FileSystemSession{
                    .engine = engine_,
                    .dialect = dialect_,
                    .homeDirectory = login->homeDirectory,
                    .parallelTransfers = transferConnections,
                },
                thread_.createStrand(),
                std::move(options)
            );
            return fileSystem_;
        }

        static Script loggedInScript()
        {
            Script script{};
            loggedIn(script);
            return script;
        }

        template <typename OperationT>
        RemoteIo::OperationResult<OperationT> later(OperationT operation)
        {
            auto result = fileSystem_->later(std::move(operation));
            return valueOf(result);
        }

        template <typename OperationT>
        RemoteIo::OperationResult<OperationT> inStrand(OperationT operation)
        {
            auto result = thread_.pushPromiseTask(
                [fileSystem = fileSystem_, operation = std::move(operation)]() mutable
                {
                    return fileSystem->inStrand(std::move(operation));
                }
            );
            return valueOf(result);
        }

        ExecutorHost host_{GetParam()};
        RemoteIo::ProcessingThread thread_{};
        std::shared_ptr<TaskGate> gate_ = std::make_shared<TaskGate>(thread_);
        std::shared_ptr<ServerDialect> dialect_ = std::make_shared<ServerDialect>();
        std::unique_ptr<ScriptedFtpServer> server_{};
        std::shared_ptr<Engine> engine_{};
        std::shared_ptr<FtpFileSystem> fileSystem_{};
    };

    TEST_P(FtpFileSystemTests, ListsTheEntriesOfADirectoryWithTheirTypes)
    {
        auto script = loggedInScript();
        listsIn(script, "/dir", std::string{listedFile} + std::string{listedDirectory}).expectDisconnect();
        connect(std::move(script));

        const auto listed = later(RemoteIo::ListDirectory{.path = remote("/dir"), .resolveLinks = false});
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(namesOf(*listed), (std::vector<std::string>{"a", "b"}));
        EXPECT_TRUE(listed->at(0u).isRegularFile());
        EXPECT_EQ(listed->at(0u).size, 5u);
        EXPECT_TRUE(listed->at(1u).isDirectory());
    }

    TEST_P(FtpFileSystemTests, ResolvesWhereLinksLeadWhenAsked)
    {
        auto script = loggedInScript();
        listsIn(script, "/dir", std::string{listedLinkToDirectory} + std::string{listedDanglingLink});
        script.expect("SIZE", "/dir/up").reply("550 Could not get file size.");
        enters(script, "/dir/up");
        script.expect("SIZE", "/dir/gone").reply("550 Could not get file size.");
        // The listing told that it is a link: its directory is not listed again to look it up.
        script.expect("CWD", "/dir/gone").reply("550 Failed to change directory.").expectDisconnect();
        connect(std::move(script));

        const auto listed = later(RemoteIo::ListDirectory{.path = remote("/dir"), .resolveLinks = true});
        ASSERT_TRUE(listed) << listed.error().toString();
        ASSERT_EQ(namesOf(*listed), (std::vector<std::string>{"up", "gone"}));
        EXPECT_TRUE(listed->at(0u).isDirectoryLike());
        EXPECT_EQ(listed->at(0u).linkTarget, "sub");
        EXPECT_EQ(listed->at(1u).resolvedTarget, nullptr) << "a dangling link got a target";
        EXPECT_EQ(listed->at(1u).linkTarget, "none");
    }

    TEST_P(FtpFileSystemTests, RunsInStrandOnTheProcessingThread)
    {
        auto script = loggedInScript();
        listsIn(script, "/dir", std::string{listedFile}).expectDisconnect();
        connect(std::move(script));

        const auto entry = inStrand(RemoteIo::Lstat{.path = remote("/dir/a")});
        ASSERT_TRUE(entry) << entry.error().toString();
        EXPECT_EQ(entry->name, "a");
        EXPECT_TRUE(entry->isRegularFile());
        EXPECT_EQ(entry->size, 5u);
    }

    TEST_P(FtpFileSystemTests, BrowsingDoesNotWaitForTheTasksOfTheSession)
    {
        auto script = loggedInScript();
        listsIn(script, "/dir", std::string{listedFile}).expectDisconnect();
        const auto fileSystem = connect(std::move(script));

        std::promise<void> letGo{};
        auto held = thread_.pushPromiseTask(
            [waiting = letGo.get_future().share()]()
            {
                waiting.wait();
            }
        );
        auto listed = fileSystem->later(RemoteIo::ListDirectory{.path = remote("/dir")});
        const auto answered = listed.wait_for(10s) == std::future_status::ready;
        letGo.set_value();
        held.wait();
        ASSERT_TRUE(answered) << "the listing waited for the task that holds the processing thread";
        const auto result = listed.get();
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(namesOf(*result), (std::vector<std::string>{"a"}));
    }

    TEST_P(FtpFileSystemTests, OperationsAtOnceTakeTurnsOnTheConnectionForCommands)
    {
        auto script = loggedInScript();
        listsIn(script, "/one", std::string{listedFile});
        listsIn(script, "/two", std::string{listedDirectory}).expectDisconnect();
        const auto fileSystem = connect(std::move(script));

        auto first = fileSystem->later(RemoteIo::ListDirectory{.path = remote("/one")});
        auto second = fileSystem->later(RemoteIo::ListDirectory{.path = remote("/two")});
        const auto one = valueOf(first);
        const auto two = valueOf(second);
        ASSERT_TRUE(one) << one.error().toString();
        ASSERT_TRUE(two) << two.error().toString();
        EXPECT_EQ(namesOf(*one), (std::vector<std::string>{"a"}));
        EXPECT_EQ(namesOf(*two), (std::vector<std::string>{"b"}));
    }

    TEST_P(FtpFileSystemTests, AnswersWhatTheLoginToldWithoutAskingTheServer)
    {
        auto script = loggedInScript();
        script.expectDisconnect();
        const auto fileSystem = connect(std::move(script));

        const auto home = inStrand(RemoteIo::HomeDirectory{});
        ASSERT_TRUE(home) << home.error().toString();
        EXPECT_EQ(home->value(), "/home/user");
        const auto canonicalHome = later(RemoteIo::CanonicalHomeDirectory{});
        ASSERT_TRUE(canonicalHome) << canonicalHome.error().toString();
        EXPECT_EQ(canonicalHome->value(), "/home/user");
        const auto limits = later(RemoteIo::ReadTransferLimits{});
        ASSERT_TRUE(limits) << limits.error().toString();
        EXPECT_EQ(limits->maxWriteLength, 0u);
        const auto link = later(RemoteIo::CreateSymLink{.target = "a", .linkPath = remote("/dir/link")});
        ASSERT_FALSE(link) << "created a link";
        EXPECT_EQ(link.error().kind, RemoteIo::RemoteErrorKind::Unsupported);

        const auto capabilities = fileSystem->capabilities();
        EXPECT_EQ(capabilities.maxParallelTransfers, 3u);
        EXPECT_TRUE(capabilities.preferAsyncTransfers);
        EXPECT_EQ(capabilities.renameSemantics, RemoteIo::RenameSemantics::Unknown);
        EXPECT_FALSE(capabilities.hardLinks);
    }

    TEST_P(FtpFileSystemTests, RemovesFilesAndEmptyDirectoriesAlike)
    {
        auto script = loggedInScript();
        script.expect("DELE", "/dir/a").reply("250 Delete operation successful.");
        script.expect("DELE", "/dir/b").reply("550 Delete operation failed.");
        listsIn(script, "/dir", std::string{listedDirectory});
        enters(script, "/dir");
        script.expect("RMD", "/dir/b").reply("250 Remove directory operation successful.").expectDisconnect();
        connect(std::move(script));

        const auto removed = later(RemoteIo::RemoveAll{.paths = {remote("/dir/a"), remote("/dir/b")}});
        EXPECT_TRUE(removed) << removed.error().toString();
    }

    TEST_P(FtpFileSystemTests, RemovingStopsAtTheFirstFailure)
    {
        auto script = loggedInScript();
        script.expect("DELE", "/dir/a").reply("550 /dir/a: Permission denied").expectDisconnect();
        connect(std::move(script));

        const auto removed = later(RemoteIo::RemoveAll{.paths = {remote("/dir/a"), remote("/dir/b")}});
        ASSERT_FALSE(removed) << "removed a file the server refused";
        EXPECT_EQ(removed.error().kind, RemoteIo::RemoteErrorKind::PermissionDenied) << removed.error().toString();
    }

    TEST_P(FtpFileSystemTests, FiltersOutEmptyDirectoriesAndWhatIsNoDirectory)
    {
        auto script = loggedInScript();
        listsIn(script, "/full", std::string{listedFile});
        listsIn(script, "/empty", "");
        script.expect("CWD", "/file").reply("550 Failed to change directory.");
        listsIn(script, "/", "-rw-r--r--    1 0        0               5 Jan 02 03:04 file\r\n").expectDisconnect();
        connect(std::move(script));

        const auto withContent = later(
            RemoteIo::FilterOutEmptyDirectories{.directories = {remote("/full"), remote("/empty"), remote("/file")}}
        );
        ASSERT_TRUE(withContent) << withContent.error().toString();
        EXPECT_EQ(withContent->size(), 1u);
        EXPECT_EQ(withContent->front().value(), "/full");
    }

    TEST_P(FtpFileSystemTests, CreatesAMissingDirectoryAndKeepsOneThatExists)
    {
        auto script = loggedInScript();
        script.expect("MKD", "/dir/new").reply("257 \"/dir/new\" created");
        script.expect("MKD", "/dir/b").reply("550 Create directory operation failed.");
        listsIn(script, "/dir", std::string{listedDirectory});
        script.expect("SIZE", "/dir/b").reply("550 Could not get file size.");
        enters(script, "/dir/b");
        script.expect("MKD", "/dir/a").reply("550 Create directory operation failed.");
        listsIn(script, "/dir", std::string{listedFile});
        script.expect("SIZE", "/dir/a").reply("213 5");
        script.expect("MDTM", "/dir/a").reply("213 20200102030405").expectDisconnect();
        connect(std::move(script));

        const auto created = later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = remote("/dir/new")});
        EXPECT_TRUE(created) << created.error().toString();
        const auto kept = later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = remote("/dir/b")});
        EXPECT_TRUE(kept) << kept.error().toString();
        const auto file = later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = remote("/dir/a")});
        ASSERT_FALSE(file) << "took a file for a directory";
        EXPECT_EQ(file.error().kind, RemoteIo::RemoteErrorKind::AlreadyExists) << file.error().toString();
    }

    TEST_P(FtpFileSystemTests, AFileIsCanonicalThroughItsParent)
    {
        auto script = loggedInScript();
        script.expect("CWD", "/dir/a").reply("550 Failed to change directory.");
        listsIn(script, "/dir", std::string{listedFile});
        listsIn(script, "/dir", std::string{listedFile});
        enters(script, "/dir");
        script.expect("PWD").reply("257 \"/srv/dir\" is the current directory").expectDisconnect();
        connect(std::move(script));

        const auto canonical = later(RemoteIo::CanonicalPath{.path = remote("/dir/a")});
        ASSERT_TRUE(canonical) << canonical.error().toString();
        EXPECT_EQ(canonical->value(), "/srv/dir/a");
    }

    TEST_P(FtpFileSystemTests, LinkingIntoPlaceIsUnsupportedOnlyWhereTheDestinationIsFree)
    {
        auto script = loggedInScript();
        listsIn(script, "/dir", std::string{listedFile});
        listsIn(script, "/dir", std::string{listedFile}).expectDisconnect();
        connect(std::move(script));

        const auto free = later(RemoteIo::LinkIntoPlace{.source = remote("/dir/a"), .destination = remote("/dir/new")});
        ASSERT_FALSE(free) << "linked without hard links";
        EXPECT_EQ(free.error().kind, RemoteIo::RemoteErrorKind::Unsupported) << free.error().toString();
        const auto taken = later(RemoteIo::LinkIntoPlace{.source = remote("/dir/new"), .destination = remote("/dir/a")});
        ASSERT_FALSE(taken) << "linked over a file";
        EXPECT_EQ(taken.error().kind, RemoteIo::RemoteErrorKind::AlreadyExists) << taken.error().toString();
    }

    TEST_P(FtpFileSystemTests, LinksAreReadAndFollowedAsTheListingNamesThem)
    {
        const auto parentListing = std::string{listedLinkToDirectory} + std::string{listedSubdirectory};
        auto script = loggedInScript();
        listsIn(script, "/dir", parentListing);
        listsIn(script, "/dir", parentListing);
        listsIn(script, "/dir", parentListing).expectDisconnect();
        connect(std::move(script));

        const auto content = later(RemoteIo::ReadLink{.path = remote("/dir/up")});
        ASSERT_TRUE(content) << content.error().toString();
        EXPECT_EQ(*content, "sub");
        const auto deep = later(RemoteIo::ReadLinkDeep{.path = remote("/dir/up")});
        ASSERT_TRUE(deep) << deep.error().toString();
        EXPECT_EQ(deep->linkContent, "sub");
        EXPECT_EQ(deep->linkTarget.value(), "/dir/sub");
        ASSERT_TRUE(deep->targetInfo);
        EXPECT_TRUE(deep->targetInfo->isDirectory());
    }

    TEST_P(FtpFileSystemTests, CloseFailsTheOperationsUnderWayAndRefusesLaterOnes)
    {
        auto script = loggedInScript();
        script.expect("CWD", "/slow").checkpoint("entering").awaitRelease("answer");
        script.reply("250 Directory successfully changed.");
        listingOf(script, "LIST", "-a", std::string{listedFile}).expectDisconnect();
        const auto fileSystem = connect(std::move(script));

        auto completions = std::make_shared<Completions<RemoteIo::OperationResult<RemoteIo::ListDirectory>>>();
        auto arrived = completions->arrived.get_future();
        fileSystem->later(
            RemoteIo::ListDirectory{.path = remote("/slow")},
            [completions](RemoteIo::OperationResult<RemoteIo::ListDirectory> result)
            {
                completions->record(std::move(result));
            }
        );
        ASSERT_TRUE(server_->waitForCheckpoint("entering"));

        EXPECT_TRUE(fileSystem->close());
        ASSERT_EQ(arrived.wait_for(10s), std::future_status::ready) << "the close did not end the listing";
        {
            std::scoped_lock lock{completions->mutex};
            ASSERT_FALSE(*completions->first) << "listed after the close";
            EXPECT_EQ(completions->first->error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        }
        EXPECT_FALSE(fileSystem->close()) << "closed twice";
        EXPECT_TRUE(fileSystem->strand()->isFinalized());
        const auto refused = later(RemoteIo::HomeDirectory{});
        ASSERT_FALSE(refused) << "answered after the close";
        EXPECT_EQ(refused.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);

        // The listing still runs to its end on the engine, its result is not handed on.
        server_->release("answer");
        ASSERT_TRUE(engine_->waitIdle(10s));
        std::scoped_lock lock{completions->mutex};
        EXPECT_EQ(completions->count, 1);
    }

    TEST_P(FtpFileSystemTests, AnOperationTheServerDoesNotAnswerEndsAtItsBackstop)
    {
        auto script = loggedInScript();
        script.expect("CWD", "/stuck").awaitRelease("answer");
        script.reply("250 Directory successfully changed.").expectDisconnect();
        connect(
            std::move(script),
            FtpFileSystemOptions{.operationLimits = SyncLimits{.backstop = 200ms, .drain = 5s}}
        );

        const auto begin = std::chrono::steady_clock::now();
        const auto listed = inStrand(RemoteIo::ListDirectory{.path = remote("/stuck")});
        ASSERT_FALSE(listed) << "listed what the server did not answer";
        EXPECT_EQ(listed.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << listed.error().toString();
        EXPECT_GE(std::chrono::steady_clock::now() - begin, 200ms);
        server_->release("answer");
    }

    TEST_P(FtpFileSystemTests, TheBackstopEndsAListingWhileItLooksUpLinks)
    {
        auto script = loggedInScript();
        listsIn(script, "/dir", std::string{listedLinkToDirectory} + std::string{listedDanglingLink});
        script.expect("SIZE", "/dir/up").awaitRelease("answer");
        script.reply("550 Could not get file size.").expectDisconnect();
        connect(
            std::move(script),
            FtpFileSystemOptions{.operationLimits = SyncLimits{.backstop = 300ms, .drain = 5s}}
        );

        const auto listed = later(RemoteIo::ListDirectory{.path = remote("/dir"), .resolveLinks = true});
        ASSERT_FALSE(listed) << "listed past the backstop";
        EXPECT_EQ(listed.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << listed.error().toString();
        server_->release("answer");
    }

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        FtpFileSystemTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );

    namespace
    {
        /**
         * @brief A completion of an operation whose result lands in @p results.
         */
        RemoteIo::Completion<RemoteIo::HomeDirectory>
        completionInto(std::shared_ptr<std::vector<RemoteIo::OperationResult<RemoteIo::HomeDirectory>>> results)
        {
            return RemoteIo::Completion<RemoteIo::HomeDirectory>{
                [results = std::move(results)](RemoteIo::OperationResult<RemoteIo::HomeDirectory> result)
                {
                    results->push_back(std::move(result));
                }
            };
        }

        RemoteIo::RemoteError closedFileSystem()
        {
            return RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::ConnectionLost, .message = "closed"};
        }
    }

    TEST(RunningOperationsTests, CloseFailsWhatRunsOnceAndRefusesLaterOperations)
    {
        const auto running = std::make_shared<RunningOperations>();
        const auto results = std::make_shared<std::vector<RemoteIo::OperationResult<RemoteIo::HomeDirectory>>>();
        const auto first = std::make_shared<TrackedCompletion<RemoteIo::HomeDirectory>>(completionInto(results));
        ASSERT_TRUE(first->track(running, first));
        EXPECT_EQ(running->size(), 1u);

        EXPECT_TRUE(running->close(closedFileSystem()));
        ASSERT_EQ(results->size(), 1u);
        ASSERT_FALSE(results->front());
        EXPECT_EQ(results->front().error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        first->complete(Paths::RemotePath::root());
        EXPECT_EQ(results->size(), 1u) << "completed twice";

        const auto later = std::make_shared<TrackedCompletion<RemoteIo::HomeDirectory>>(completionInto(results));
        EXPECT_FALSE(later->track(running, later));
        EXPECT_FALSE(running->close(closedFileSystem()));
    }

    TEST(RunningOperationsTests, ACompletedOperationLeavesAndIsNotFailedByTheClose)
    {
        const auto running = std::make_shared<RunningOperations>();
        const auto results = std::make_shared<std::vector<RemoteIo::OperationResult<RemoteIo::HomeDirectory>>>();
        const auto operation = std::make_shared<TrackedCompletion<RemoteIo::HomeDirectory>>(completionInto(results));
        ASSERT_TRUE(operation->track(running, operation));
        operation->complete(Paths::RemotePath::root());
        EXPECT_EQ(running->size(), 0u);

        running->close(closedFileSystem());
        ASSERT_EQ(results->size(), 1u);
        EXPECT_TRUE(results->front());
    }

    TEST(RunningOperationsTests, ADroppedOperationCompletesAsUnfinishedAndLeaves)
    {
        const auto running = std::make_shared<RunningOperations>();
        const auto results = std::make_shared<std::vector<RemoteIo::OperationResult<RemoteIo::HomeDirectory>>>();
        {
            const auto operation =
                std::make_shared<TrackedCompletion<RemoteIo::HomeDirectory>>(completionInto(results));
            ASSERT_TRUE(operation->track(running, operation));
        }
        EXPECT_EQ(running->size(), 0u);
        ASSERT_EQ(results->size(), 1u);
        EXPECT_FALSE(results->front()) << "a dropped operation succeeded";
    }
}
