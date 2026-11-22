#pragma once

#ifndef _WIN32
#    include "test_engine.hpp"
#    include "test_metadata_conformance.hpp"

#    include <ftp/remote/ftp_file_system.hpp>
#    include <ftp/support/session_start.hpp>
#    include <paths/remote_path.hpp>
#    include <remote_io/async/processing_thread.hpp>
#    include <remote_io/async/transfer_context.hpp>

#    include <gtest/gtest.h>

#    include <atomic>
#    include <chrono>
#    include <future>
#    include <memory>
#    include <mutex>
#    include <set>
#    include <string>
#    include <utility>
#    include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief The file system and its streams against every selected server, plain and over both kinds of TLS, on an
     *        engine with a pool of two threads, each case in a copy of the fixtures of its own.
     */
    class FileSystemConformance : public LoginConformance
    {
      protected:
        using SignedSize = RemoteIo::IRemoteFileStream::SignedSizeType;

        void SetUp() override
        {
            LoginConformance::SetUp();
            if (IsSkipped() || HasFatalFailure())
                return;
            auto workspace = prepareWorkspace(endpoint_);
            ASSERT_TRUE(workspace.has_value()) << workspace.error();
            workspace_ = std::move(workspace).value();
            thread_.start();

            auto loginOptions = options();
            if (mode() != TlsMode::None)
            {
                const auto probe = makeConnection();
                const auto refused = login(probe, options());
                ASSERT_FALSE(refused);
                ASSERT_TRUE(probe->untrustedCertificate());
                loginOptions = options(probe->untrustedCertificate()->publicKeySha256);
            }
            auto started = startSession(
                thread_,
                SessionStart{
                    .executor = engineHost_.executor(),
                    .gate = gate_,
                    .pool =
                        PoolOptions{
                            .host = endpoint_.address,
                            .port = port(),
                            .login = std::move(loginOptions),
                            .transferConnections = 2u,
                        },
                    .loginLimit = 30s,
                }
            );
            ASSERT_TRUE(started.has_value()) << started.error();
            engine_ = std::move(started->engine);
            fileSystem_ = std::move(started->fileSystem);
        }

        void TearDown() override
        {
            if (fileSystem_)
                fileSystem_->close();
            fileSystem_.reset();
            if (engine_)
            {
                EXPECT_FALSE(engine_->failed()) << "an operation failed the session";
                engine_->close();
            }
            thread_.stop();
            engine_.reset();
            LoginConformance::TearDown();
            expectWorkspaceRemoved(endpoint_, workspace_);
        }

        Paths::RemotePath remotePathOf(std::string_view name) const
        {
            return Paths::RemotePath::make(fmt::format("{}/{}", workspace_, name)).value();
        }

        template <typename OperationT>
        RemoteIo::OperationResult<OperationT> later(OperationT operation)
        {
            auto result = fileSystem_->later(std::move(operation));
            if (result.wait_for(60s) != std::future_status::ready)
                throw std::runtime_error{"The operation did not end"};
            return result.get();
        }

        template <typename FunctionT>
        auto onThread(FunctionT function)
        {
            auto result = thread_.pushPromiseTask(std::move(function));
            if (result.wait_for(60s) != std::future_status::ready)
                throw std::runtime_error{"The task did not end"};
            return result.get();
        }

        std::shared_ptr<RemoteIo::IRemoteFileStream> open(std::string_view name, RemoteIo::OpenMode mode)
        {
            const auto opened = later(RemoteIo::OpenFile{.path = remotePathOf(name), .mode = mode});
            if (!opened)
                throw std::runtime_error{fmt::format("Opening {} failed: {}", name, opened.error().toString())};
            auto stream = opened->lock();
            if (!stream)
                throw std::runtime_error{"The stream was gone after the open"};
            return stream;
        }

        /**
         * @brief What an asynchronous transfer of a case moved and how it ended.
         */
        struct Transfer
        {
            std::mutex mutex{};
            std::string content{};
            std::vector<char> buffer = std::vector<char>(64u * 1024u);
            std::atomic<bool> endNoted{false};
            std::promise<void> ended{};
            std::future<void> endedFuture{ended.get_future()};
        };

        using ContextResult = std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>;

        static void observe(RemoteIo::AsyncTransferContext& context, std::shared_ptr<Transfer> const& transfer)
        {
            context.observe(
                [transfer, observed = &context]()
                {
                    if (observed->hasEnded() && !transfer->endNoted.exchange(true))
                        transfer->ended.set_value();
                }
            );
        }

        /**
         * @brief Reads @p total bytes of @p stream asynchronously and closes it, after @p before on the thread.
         */
        std::expected<std::string, RemoteIo::RemoteError> readAsync(
            std::shared_ptr<RemoteIo::IRemoteFileStream> const& stream,
            SignedSize total,
            std::function<std::expected<void, RemoteIo::RemoteError>()> before = {}
        )
        {
            const auto transfer = std::make_shared<Transfer>();
            const auto started = onThread(
                [stream, total, transfer, before]() -> ContextResult
                {
                    if (before)
                    {
                        if (auto prepared = before(); !prepared)
                            return std::unexpected(prepared.error());
                    }
                    auto context = stream->readAsyncInStrand(
                        total,
                        transfer->buffer.data(),
                        static_cast<SignedSize>(transfer->buffer.size()),
                        [transfer](SignedSize count)
                        {
                            std::scoped_lock lock{transfer->mutex};
                            transfer->content.append(transfer->buffer.data(), static_cast<std::size_t>(count));
                            return true;
                        }
                    );
                    if (context)
                        observe(**context, transfer);
                    return context;
                }
            );
            return endOf(stream, started, transfer).transform(
                [transfer]()
                {
                    std::scoped_lock lock{transfer->mutex};
                    return transfer->content;
                }
            );
        }

        std::expected<void, RemoteIo::RemoteError>
        writeAsync(std::shared_ptr<RemoteIo::IRemoteFileStream> const& stream, std::string content)
        {
            const auto transfer = std::make_shared<Transfer>();
            transfer->content = std::move(content);
            const auto started = onThread(
                [stream, transfer]() -> ContextResult
                {
                    auto context = stream->writeAsyncInStrand(
                        static_cast<SignedSize>(transfer->content.size()),
                        transfer->buffer.data(),
                        static_cast<SignedSize>(transfer->buffer.size()),
                        [transfer](SignedSize wanted)
                        {
                            std::scoped_lock lock{transfer->mutex};
                            const auto count = std::min(static_cast<std::size_t>(wanted), transfer->content.size());
                            std::copy_n(transfer->content.begin(), count, transfer->buffer.begin());
                            transfer->content.erase(0u, count);
                            return static_cast<SignedSize>(count);
                        }
                    );
                    if (context)
                        observe(**context, transfer);
                    return context;
                }
            );
            return endOf(stream, started, transfer);
        }

        /**
         * @brief Waits for the end of the transfer @p started, finishes and closes @p stream.
         */
        std::expected<void, RemoteIo::RemoteError> endOf(
            std::shared_ptr<RemoteIo::IRemoteFileStream> const& stream,
            ContextResult const& started,
            std::shared_ptr<Transfer> const& transfer
        )
        {
            if (!started)
                return std::unexpected(started.error());
            if (transfer->endedFuture.wait_for(60s) != std::future_status::ready)
                throw std::runtime_error{"The transfer did not end"};
            const auto finished = onThread(
                [stream, context = *started]() -> std::expected<void, RemoteIo::RemoteError>
                {
                    stream->closeInStrand();
                    if (context->outcome() != RemoteIo::AsyncTransferContext::Outcome::Completed)
                        return std::unexpected(context->error().value_or(RemoteIo::RemoteError{.message = "cancelled"}));
                    return {};
                }
            );
            return finished;
        }

        std::string workspace_{};
        ExecutorHost engineHost_{EngineExecutor::ThreadPool};
        RemoteIo::ProcessingThread thread_{};
        std::shared_ptr<TaskGate> gate_ = std::make_shared<TaskGate>(thread_);
        std::shared_ptr<Engine> engine_{};
        std::shared_ptr<FtpFileSystem> fileSystem_{};
    };

    TEST_P(FileSystemConformance, ListsTheWorkspaceWithWhereItsLinksLead)
    {
        const auto listed = later(RemoteIo::ListDirectory{.path = remotePathOf(""), .resolveLinks = true});
        ASSERT_TRUE(listed) << listed.error().toString();
        std::set<std::string> names{};
        for (auto const& entry : *listed)
            names.insert(entry.name);
        EXPECT_EQ(names, workspaceNames());
        for (auto const& entry : *listed)
        {
            if (entry.name == "link-to-directory")
                EXPECT_TRUE(entry.isDirectoryLike()) << entry.name;
            if (entry.name == "link-to-plain.txt")
            {
                ASSERT_TRUE(entry.resolvedTarget) << entry.name;
                EXPECT_TRUE(entry.resolvedTarget->isRegularFile()) << entry.name;
            }
            if (entry.name == "dangling-link")
                EXPECT_EQ(entry.resolvedTarget, nullptr) << "a dangling link got a target";
        }
    }

    TEST_P(FileSystemConformance, DownloadsAsynchronouslyAndAfterATailCheck)
    {
        constexpr std::size_t size = 10485760u;
        const auto expected = fixtureContent(size);
        const auto whole = readAsync(open("large.bin", RemoteIo::OpenMode::Read), static_cast<SignedSize>(size));
        ASSERT_TRUE(whole) << whole.error().toString();
        EXPECT_TRUE(*whole == expected) << "the download differs, " << whole->size() << " bytes";

        // A resume: the tail before the offset is read synchronously, the rest goes on with the same RETR.
        constexpr std::size_t offset = 4u * 1024u * 1024u;
        constexpr std::size_t tail = 4096u;
        const auto stream = open("large.bin", RemoteIo::OpenMode::Read);
        std::string read{};
        const auto rest = readAsync(
            stream,
            static_cast<SignedSize>(size - offset),
            [&stream, &read]() -> std::expected<void, RemoteIo::RemoteError>
            {
                if (auto sought = stream->seekInStrand(offset - tail); !sought)
                    return sought;
                std::vector<char> buffer(tail);
                while (read.size() < tail)
                {
                    const auto got = stream->readSomeInStrand(buffer.data(), tail - read.size());
                    if (!got)
                        return std::unexpected(got.error());
                    if (*got == 0u)
                        break;
                    read.append(buffer.data(), *got);
                }
                return stream->seekInStrand(offset);
            }
        );
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_TRUE(read == expected.substr(offset - tail, tail)) << "the tail differs";
        EXPECT_TRUE(*rest == expected.substr(offset)) << "the rest differs, " << rest->size() << " bytes";
    }

    TEST_P(FileSystemConformance, UploadsAsynchronouslyAndReadsTheFileBack)
    {
        const auto content = uploadContent(1024u * 1024u + 123u);
        const auto written = writeAsync(open("uploaded.bin", RemoteIo::OpenMode::Write), content);
        ASSERT_TRUE(written) << written.error().toString();
        const auto size = later(RemoteIo::Stat{.path = remotePathOf("uploaded.bin")});
        ASSERT_TRUE(size) << size.error().toString();
        EXPECT_EQ(size->size, content.size());
        const auto back = readAsync(open("uploaded.bin", RemoteIo::OpenMode::Read), static_cast<SignedSize>(content.size()));
        ASSERT_TRUE(back) << back.error().toString();
        EXPECT_TRUE(*back == content) << "the file read back differs";

        const auto small = later(RemoteIo::ReadFile{.path = remotePathOf("plain.txt")});
        ASSERT_TRUE(small) << small.error().toString();
        EXPECT_EQ(*small, "plain text\n");
    }

    TEST_P(FileSystemConformance, ChangesTheTreeWithTheOperationsOfTheFileSystem)
    {
        const auto created = later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = remotePathOf("made")});
        ASSERT_TRUE(created) << created.error().toString();
        const auto kept = later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = remotePathOf("directory")});
        EXPECT_TRUE(kept) << kept.error().toString();
        const auto onFile = later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = remotePathOf("plain.txt")});
        ASSERT_FALSE(onFile) << "took a file for a directory";
        EXPECT_EQ(onFile.error().kind, RemoteIo::RemoteErrorKind::AlreadyExists) << onFile.error().toString();

        const auto withContent = later(
            RemoteIo::FilterOutEmptyDirectories{
                .directories = {remotePathOf("made"), remotePathOf("directory"), remotePathOf("plain.txt")},
            }
        );
        ASSERT_TRUE(withContent) << withContent.error().toString();
        ASSERT_EQ(withContent->size(), 1u);
        EXPECT_EQ(withContent->front(), remotePathOf("directory"));

        const auto renamed =
            later(RemoteIo::Rename{.source = remotePathOf("one.bin"), .destination = remotePathOf("made/moved.bin")});
        ASSERT_TRUE(renamed) << renamed.error().toString();
        const auto removed =
            later(RemoteIo::RemoveAll{.paths = {remotePathOf("made/moved.bin"), remotePathOf("made")}});
        ASSERT_TRUE(removed) << removed.error().toString();
        const auto gone = later(RemoteIo::Lstat{.path = remotePathOf("made")});
        ASSERT_FALSE(gone) << "the directory is still there";
        EXPECT_EQ(gone.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile) << gone.error().toString();

        const auto canonical = later(RemoteIo::CanonicalPath{.path = remotePathOf("link-to-plain.txt")});
        ASSERT_TRUE(canonical) << canonical.error().toString();
        EXPECT_EQ(canonical->filename()->value(), "plain.txt");
        const auto deep = later(RemoteIo::ReadLinkDeep{.path = remotePathOf("link-to-plain.txt")});
        ASSERT_TRUE(deep) << deep.error().toString();
        EXPECT_EQ(deep->linkContent, "plain.txt");
        ASSERT_TRUE(deep->targetInfo);
        EXPECT_TRUE(deep->targetInfo->isRegularFile());
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        FileSystemConformance,
        ::testing::Combine(
            ::testing::ValuesIn(selectedServers()),
            ::testing::Values(TlsMode::None, TlsMode::Explicit, TlsMode::Implicit)
        ),
        [](::testing::TestParamInfo<std::tuple<std::string, TlsMode>> const& info)
        {
            const auto mode = std::get<1>(info.param);
            if (mode == TlsMode::Explicit)
                return std::get<0>(info.param) + "Explicit";
            if (mode == TlsMode::Implicit)
                return std::get<0>(info.param) + "Implicit";
            return std::get<0>(info.param) + "Plain";
        }
    );
}

#endif
