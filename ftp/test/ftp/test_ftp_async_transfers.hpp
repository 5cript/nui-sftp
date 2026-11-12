#pragma once

#include "test_ftp_file_stream.hpp"

#include <ftp/engine/ring_options.hpp>
#include <remote_io/async/transfer_context.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using SignedSize = RemoteIo::IRemoteFileStream::SignedSizeType;
        using Outcome = RemoteIo::AsyncTransferContext::Outcome;

        constexpr std::string_view sixteenBytes = "0123456789abcdef";

        /**
         * @brief What an asynchronous transfer of a test moved and how it ended.
         */
        struct AsyncState
        {
            explicit AsyncState(std::size_t bufferSize)
                : buffer(bufferSize)
            {}

            /**
             * @brief The observer of the context: notes its end once.
             */
            void observed(RemoteIo::AsyncTransferContext const& context)
            {
                if (!context.hasEnded() || endNoted.exchange(true))
                    return;
                ended.set_value();
            }

            std::vector<char> buffer;
            std::mutex mutex{};
            /** @brief What the reads handed over, or what is left to upload. */
            std::string content{};
            std::size_t calls{0u};
            std::atomic<bool> endNoted{false};
            std::promise<void> ended{};
            std::future<void> endedFuture{ended.get_future()};
            std::promise<void> firstCall{};
            std::future<void> firstCallFuture{firstCall.get_future()};
            /** @brief Called on the processing thread at every read, false refuses the bytes. */
            std::function<bool()> onCall{};
        };
    }

    class FtpAsyncTransferTests : public FtpFileStreamTests
    {
      protected:
        static FtpFileSystemOptions smallRing()
        {
            FtpFileSystemOptions options{};
            options.ring = RingOptions{.slots = 2u, .chunkSize = 4u};
            return options;
        }

        std::shared_ptr<FtpFileSystem> connectSmall(Script metadata, Script transfer)
        {
            const auto fileSystem = connect(std::move(metadata), smallRing());
            server_->addConnection(std::move(transfer));
            return fileSystem;
        }

        using ContextResult = std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>;

        /**
         * @brief Starts reading @p total bytes asynchronously into the content of @p state, observed as the backend
         *        does in the same task. @p prepare runs in that task too, before anything was delivered.
         */
        ContextResult readAsync(
            StreamPointer const& stream,
            SignedSize total,
            std::shared_ptr<AsyncState> const& state,
            std::function<void(RemoteIo::AsyncTransferContext&)> prepare = {}
        )
        {
            return onThread(
                [stream, total, state, prepare]() -> ContextResult
                {
                    auto context = stream->readAsyncInStrand(
                        total,
                        state->buffer.data(),
                        static_cast<SignedSize>(state->buffer.size()),
                        [state](SignedSize count)
                        {
                            std::scoped_lock lock{state->mutex};
                            state->content.append(state->buffer.data(), static_cast<std::size_t>(count));
                            if (++state->calls == 1u)
                                state->firstCall.set_value();
                            return !state->onCall || state->onCall();
                        }
                    );
                    if (!context)
                        return context;
                    auto* const observed = context->get();
                    observed->observe(
                        [state, observed]()
                        {
                            state->observed(*observed);
                        }
                    );
                    if (prepare)
                        prepare(*observed);
                    return context;
                }
            );
        }

        /**
         * @brief Starts writing the content of @p state asynchronously, observed as the backend does.
         */
        ContextResult writeAsync(
            StreamPointer const& stream,
            std::shared_ptr<AsyncState> const& state,
            std::function<void(RemoteIo::AsyncTransferContext&)> prepare = {}
        )
        {
            return onThread(
                [stream, state, prepare]() -> ContextResult
                {
                    const auto total = static_cast<SignedSize>(state->content.size());
                    auto context = stream->writeAsyncInStrand(
                        total,
                        state->buffer.data(),
                        static_cast<SignedSize>(state->buffer.size()),
                        [state](SignedSize wanted)
                        {
                            std::scoped_lock lock{state->mutex};
                            const auto count = std::min(static_cast<std::size_t>(wanted), state->content.size());
                            std::copy_n(state->content.begin(), count, state->buffer.begin());
                            state->content.erase(0u, count);
                            if (++state->calls == 1u)
                                state->firstCall.set_value();
                            if (state->onCall && !state->onCall())
                                return SignedSize{0};
                            return static_cast<SignedSize>(count);
                        }
                    );
                    if (!context)
                        return context;
                    auto* const observed = context->get();
                    observed->observe(
                        [state, observed]()
                        {
                            state->observed(*observed);
                        }
                    );
                    if (prepare)
                        prepare(*observed);
                    return context;
                }
            );
        }

        static void expectEnd(std::shared_ptr<AsyncState> const& state)
        {
            ASSERT_EQ(state->endedFuture.wait_for(10s), std::future_status::ready) << "the transfer did not end";
        }
    };

    TEST_P(FtpAsyncTransferTests, AnAsynchronousDownloadDeliversEverySlotAndCompletesAfterTheFinalReply)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "16");
        completed(transferring(transfer, DataPlan{}.send(std::string{sixteenBytes}, 5u), "RETR", "/dir/file"))
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto state = std::make_shared<AsyncState>(3u);
        const auto context = readAsync(stream, 16, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        EXPECT_EQ((*context)->outcome(), Outcome::Completed) << (*context)->error().value_or({}).toString();
        EXPECT_EQ((*context)->bytesTransferred(), 16);
        {
            std::scoped_lock lock{state->mutex};
            EXPECT_EQ(state->content, sixteenBytes);
        }
        const auto finished = onThread(
            [stream]()
            {
                return stream->finishInStrand();
            }
        );
        EXPECT_TRUE(finished) << finished.error().toString();
        closeOnThread(stream);
    }

    TEST_P(FtpAsyncTransferTests, AnAsynchronousDownloadAfterTheTailCheckGoesOnWithTheSameTransfer)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "10");
        transfer.expect("EPSV")
            .passive(DataPlan{}.send("456789"))
            .expect("REST", "4")
            .reply("350 Restart position accepted (4).");
        completed(transfer.expect("RETR", "/dir/file").reply("150 Opening BINARY mode data connection."))
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto tail = onThread(
            [stream]() -> std::expected<std::string, RemoteIo::RemoteError>
            {
                if (auto sought = stream->seekInStrand(4u); !sought)
                    return std::unexpected(sought.error());
                std::array<char, 2u> buffer{};
                std::size_t filled = 0u;
                while (filled < buffer.size())
                {
                    const auto read = stream->readSomeInStrand(buffer.data() + filled, buffer.size() - filled);
                    if (!read)
                        return std::unexpected(read.error());
                    if (*read == 0u)
                        break;
                    filled += *read;
                }
                if (auto sought = stream->seekInStrand(6u); !sought)
                    return std::unexpected(sought.error());
                return std::string{buffer.data(), filled};
            }
        );
        ASSERT_TRUE(tail) << tail.error().toString();
        EXPECT_EQ(*tail, "45");
        const auto state = std::make_shared<AsyncState>(3u);
        const auto context = readAsync(stream, 4, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        EXPECT_EQ((*context)->outcome(), Outcome::Completed) << (*context)->error().value_or({}).toString();
        std::scoped_lock lock{state->mutex};
        EXPECT_EQ(state->content, "6789");
    }

    TEST_P(FtpAsyncTransferTests, CancellingAnAsynchronousDownloadAbortsIt)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "16");
        transferring(transfer, DataPlan{}.stallAfter("0123"), "RETR", "/dir/file")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto state = std::make_shared<AsyncState>(4u);
        const auto context = readAsync(stream, 16, state);
        ASSERT_TRUE(context) << context.error().toString();
        ASSERT_EQ(state->firstCallFuture.wait_for(10s), std::future_status::ready) << "nothing was delivered";
        const auto overlapping = onThread(
            [stream]()
            {
                std::array<char, 4u> buffer{};
                return stream->readSomeInStrand(buffer.data(), buffer.size());
            }
        );
        ASSERT_FALSE(overlapping) << "read beside the asynchronous transfer";
        EXPECT_EQ(overlapping.error().kind, RemoteIo::RemoteErrorKind::Failure) << overlapping.error().toString();
        (*context)->cancel();
        expectEnd(state);
        EXPECT_EQ((*context)->outcome(), Outcome::Canceled);
        // The ABOR goes out without the close, which only gives the connection back.
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
        closeOnThread(stream);
    }

    TEST_P(FtpAsyncTransferTests, ADownloadThatEndsShortOfTheExpectedSizeFails)
    {
        // The SIZE after the short data says the file changed, which the download takes, the stream does not.
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "10");
        completed(transferring(transfer, DataPlan{}.send("012345"), "RETR", "/dir/file"));
        sized(transfer, "/dir/file", "6").expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto state = std::make_shared<AsyncState>(4u);
        const auto context = readAsync(stream, 10, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        ASSERT_EQ((*context)->outcome(), Outcome::Failed) << "a cut download completed";
        ASSERT_TRUE((*context)->error());
        EXPECT_EQ((*context)->error()->kind, RemoteIo::RemoteErrorKind::Truncated) << (*context)->error()->toString();
        closeOnThread(stream);
    }

    TEST_P(FtpAsyncTransferTests, BytesTheReadRefusesFailTheDownloadAndAbortIt)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "16");
        transferring(transfer, DataPlan{}.stallAfter("0123"), "RETR", "/dir/file")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto state = std::make_shared<AsyncState>(4u);
        state->onCall = []()
        {
            return false;
        };
        const auto context = readAsync(stream, 16, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        ASSERT_EQ((*context)->outcome(), Outcome::Failed);
        std::scoped_lock lock{state->mutex};
        EXPECT_EQ(state->calls, 1u) << "read on after the bytes were refused";
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpAsyncTransferTests, ClosingTheStreamInsideADeliveryEndsLaterDeliveries)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "16");
        transferring(transfer, DataPlan{}.stallAfter(std::string{sixteenBytes}), "RETR", "/dir/file")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        // Two reads to a slot, the second must not come once the first closed the stream.
        const auto state = std::make_shared<AsyncState>(2u);
        state->onCall = [weak = std::weak_ptr<RemoteIo::IRemoteFileStream>{stream}]()
        {
            if (const auto locked = weak.lock())
                locked->closeInStrand();
            return true;
        };
        const auto context = readAsync(stream, 16, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        EXPECT_NE((*context)->outcome(), Outcome::Completed);
        // Every delivery that was pushed already has run once the thread got to this task.
        onThread([]() {});
        std::scoped_lock lock{state->mutex};
        EXPECT_EQ(state->calls, 1u) << "a delivery reached the read after the close";
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpAsyncTransferTests, ADownloadPausedPastTheDropGoesOnOnceResumed)
    {
        // The drop of the paused transfer shows the ring waits paused, only the resume can wake it then.
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "16");
        transferring(transfer, DataPlan{}.stallAfter("abc"), "RETR", "/dir/file")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("dropped");
        completed(transferring(transfer, DataPlan{}.send(std::string{sixteenBytes}), "RETR", "/dir/file"))
            .expectDisconnect();
        auto options = smallRing();
        options.transfer.pauseDropAfter = 100ms;
        connect(std::move(metadata), std::move(options));
        server_->addConnection(std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto state = std::make_shared<AsyncState>(4u);
        const auto context = readAsync(
            stream,
            16,
            state,
            [](RemoteIo::AsyncTransferContext& started)
            {
                started.pause(true);
            }
        );
        ASSERT_TRUE(context) << context.error().toString();
        ASSERT_TRUE(server_->waitForCheckpoint("dropped"));
        (*context)->pause(false);
        expectEnd(state);
        EXPECT_EQ((*context)->outcome(), Outcome::Completed) << (*context)->error().value_or({}).toString();
        {
            std::scoped_lock lock{state->mutex};
            EXPECT_EQ(state->content, sixteenBytes);
        }
        closeOnThread(stream);
    }

    TEST_P(FtpAsyncTransferTests, AnAsynchronousUploadWritesEverySlotAndCompletesAfterTheFinalReply)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        completed(transferring(transfer, DataPlan{}.expectUpload(1024u), "STOR", "/dir/new")).expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/new", writeMode);
        const auto state = std::make_shared<AsyncState>(3u);
        state->content = std::string{sixteenBytes};
        const auto context = writeAsync(stream, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        EXPECT_EQ((*context)->outcome(), Outcome::Completed) << (*context)->error().value_or({}).toString();
        EXPECT_EQ((*context)->bytesTransferred(), 16);
        closeOnThread(stream);
        EXPECT_EQ(uploadedContent(server_->transcript()), std::optional<std::string>{std::string{sixteenBytes}});
    }

    TEST_P(FtpAsyncTransferTests, CancellingAPausedUploadAbortsIt)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        transferring(transfer, DataPlan{}, "STOR", "/dir/new")
            .abortSequence(AbortReplies::ProftpdPlainLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/new", writeMode);
        const auto state = std::make_shared<AsyncState>(3u);
        state->content = std::string{sixteenBytes};
        const auto context = writeAsync(
            stream,
            state,
            [](RemoteIo::AsyncTransferContext& started)
            {
                started.pause(true);
            }
        );
        ASSERT_TRUE(context) << context.error().toString();
        (*context)->cancel();
        expectEnd(state);
        EXPECT_EQ((*context)->outcome(), Outcome::Canceled);
        closeOnThread(stream);
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpAsyncTransferTests, AnUploadWhoseLocalFileEndsEarlyFailsAndIsAborted)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        transferring(transfer, DataPlan{}, "STOR", "/dir/new")
            .abortSequence(AbortReplies::ProftpdPlainLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connectSmall(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/new", writeMode);
        const auto state = std::make_shared<AsyncState>(3u);
        state->content = std::string{sixteenBytes};
        state->onCall = []()
        {
            return false;
        };
        const auto context = writeAsync(stream, state);
        ASSERT_TRUE(context) << context.error().toString();
        expectEnd(state);
        ASSERT_EQ((*context)->outcome(), Outcome::Failed) << "an upload short of its size completed";
        closeOnThread(stream);
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpAsyncTransferTests, AStreamClosedWhileItsDownloadStartsKeepsTheConnectionUntilTheTransferEnded)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "16");
        transfer.expect("EPSV")
            .passive(DataPlan{}.stallAfter("0123"))
            .expect("RETR", "/dir/a")
            .checkpoint("retrieving")
            .awaitRelease("answered")
            .reply("150 Opening BINARY mode data connection.")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory);
        sized(transfer, "/dir/b", "1").expectDisconnect();
        connect(std::move(metadata), smallRing(), 1u);
        server_->addConnection(std::move(transfer));

        const auto stream = open("/dir/a", readMode);
        const auto state = std::make_shared<AsyncState>(4u);
        const auto context = readAsync(stream, 16, state);
        ASSERT_TRUE(context) << context.error().toString();
        ASSERT_TRUE(server_->waitForCheckpoint("retrieving"));
        closeOnThread(stream);
        // The RETR is out without its answer: a command of another holder would go between it and its ABOR.
        const auto early = later(RemoteIo::OpenFile{.path = remote("/dir/b"), .mode = readMode});
        ASSERT_FALSE(early) << "the connection of a transfer that was still starting was handed out";
        EXPECT_EQ(early.error().kind, RemoteIo::RemoteErrorKind::Busy) << early.error().toString();
        server_->release("answered");
        const auto next = openWhenFree("/dir/b", readMode);
        closeOnThread(next);
    }

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        FtpAsyncTransferTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );
}
