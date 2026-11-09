#pragma once

#include "test_download.hpp"

#include <ftp/client/transfer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using UploadOutcome = std::expected<UploadResult, RemoteIo::RemoteError>;

        asio::awaitable<UploadOutcome> uploadAll(
            TransferSession session,
            UploadRequest request,
            std::string content,
            TransferOptions options,
            std::size_t chunkSize
        )
        {
            auto started = co_await Upload::start(std::move(session), std::move(request), options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            auto const upload = std::move(started).value();
            for (std::size_t offset = 0u; offset < content.size(); offset += chunkSize)
            {
                const auto size = std::min(chunkSize, content.size() - offset);
                if (auto written = co_await upload->write(asio::buffer(content.data() + offset, size)); !written)
                    co_return std::unexpected(std::move(written).error());
            }
            co_return co_await upload->finish();
        }

        /**
         * @brief EPSV with @p plan behind it, then @p verb @p path answered by @p preliminary.
         */
        Script& storeWith(Script& script, DataPlan plan, std::string_view preliminary, std::string verb = "STOR")
        {
            return script.expect("EPSV").passive(std::move(plan)).expect(std::move(verb), "file").reply(preliminary);
        }

        std::optional<std::string> detailOf(std::vector<TranscriptEntry> const& transcript, TranscriptEntry::Kind kind)
        {
            const auto found = std::ranges::find_if(
                transcript,
                [kind](TranscriptEntry const& entry)
                {
                    return entry.kind == kind;
                }
            );
            if (found == transcript.end())
                return std::nullopt;
            return found->detail;
        }

        std::optional<std::string> uploadedContent(std::vector<TranscriptEntry> const& transcript)
        {
            return detailOf(transcript, TranscriptEntry::Kind::DataUploaded);
        }
    }

    class UploadTests : public DownloadTlsTests
    {
      protected:
        UploadOutcome upload(
            std::shared_ptr<ControlConnection> const& connection,
            UploadRequest request,
            std::string content,
            std::optional<TlsConnectionSettings> tls = std::nullopt,
            TransferOptions options = quickTransferOptions(),
            std::size_t chunkSize = 64u * 1024u
        )
        {
            TransferSession session{
                .control = connection,
                .passiveChoice = std::make_shared<PassiveChoice>(),
                .tls = std::move(tls),
            };
            return run(
                connection,
                [session = std::move(session),
                    request = std::move(request),
                    content = std::move(content),
                    options,
                    chunkSize]() -> asio::awaitable<UploadOutcome>
                {
                    co_return co_await uploadAll(session, request, content, options, chunkSize);
                }
            );
        }

        /**
         * @brief What the server took, once the client is gone.
         */
        std::optional<std::string> uploadedAfterDisconnect(std::shared_ptr<ControlConnection> const& connection)
        {
            connection->close();
            EXPECT_TRUE(server_->waitUntilDisconnected());
            return uploadedContent(server_->transcript());
        }

        void expectUploadError(UploadOutcome const& result, RemoteIo::RemoteErrorKind kind)
        {
            ASSERT_FALSE(result) << "uploaded " << result->bytes << " bytes";
            EXPECT_EQ(result.error().kind, kind) << result.error().toString();
        }
    };

    TEST_F(UploadTests, StoresTheFile)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.expectUpload(1u << 20u), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const std::string content(300u * 1024u, 's');
        const auto result = upload(connection, {.path = "file"}, content);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->bytes, content.size());
        ASSERT_TRUE(result->finalReply);
        EXPECT_EQ(result->finalReply->code, 226);
        EXPECT_EQ(uploadedAfterDisconnect(connection), content);
    }

    TEST_F(UploadTests, OffsetSendsRestJustBeforeStore)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{}.expectUpload(1024u))
            .expect("REST", "5")
            .reply("350 Restart position accepted (5).")
            .expect("STOR", "file")
            .reply("150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = upload(connection, {.path = "file", .offset = 5u}, "the rest");
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(uploadedAfterDisconnect(connection), std::optional<std::string>{"the rest"});
    }

    TEST_F(UploadTests, AppendSendsAppeWithoutRest)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.expectUpload(1024u), "150 Ok to send data.", "APPE")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = upload(connection, {.path = "file", .mode = UploadMode::Append, .offset = 5u}, "more");
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(uploadedAfterDisconnect(connection), std::optional<std::string>{"more"});
    }

    TEST_F(UploadTests, RefusedStoreLeavesTheControlConnectionUsable)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}, "553 Could not create file.").expect("NOOP").reply("200 Fine").expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectUploadError(upload(connection, {.path = "file"}, "x"), RemoteIo::RemoteErrorKind::PermissionDenied);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(UploadTests, MissingFinalReplyLeavesTheUploadUnconfirmed)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.expectUpload(1024u), "150 Ok to send data.").beginData().awaitData().stall();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.finalReplyTimeout = 300ms;
        const auto result = upload(connection, {.path = "file"}, "content", std::nullopt, options);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->bytes, 7u);
        EXPECT_FALSE(result->finalReply);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(UploadTests, NegativeFinalReplyFails)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.expectUpload(1024u), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("552 Exceeded storage allocation.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = upload(connection, {.path = "file"}, "content");
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().protocolCode, 552);
    }

    TEST_F(UploadTests, RefusalWhileWritingIsTheServersReason)
    {
        // The server took nothing and refused, the writes behind its close fail.
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}, "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("552 Exceeded storage allocation.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = upload(connection, {.path = "file"}, std::string(8u << 20u, 'x'));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().protocolCode, 552) << result.error().toString();
    }

    TEST_F(UploadTests, RefusalBehindTheBrokenDataIsTheServersReason)
    {
        // The delay lets the broken data connection reach the client before the refusal does.
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.reset(), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .delay(200ms)
            .reply("552 Exceeded storage allocation.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.refusalWait = 5s;
        const auto result = upload(connection, {.path = "file"}, std::string(8u << 20u, 'x'), std::nullopt, options);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().protocolCode, 552) << result.error().toString();
    }

    TEST_F(UploadTests, ConfirmedUploadWhoseDataEndFailedIsComplete)
    {
        // The server took everything and says so, then resets the data connection instead of closing it.
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.expectUpload(1024u).reset(), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = upload(connection, {.path = "file"}, "content");
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->bytes, 7u);
        ASSERT_TRUE(result->finalReply);
        EXPECT_EQ(result->finalReply->code, 226);
        EXPECT_EQ(uploadedAfterDisconnect(connection), std::optional<std::string>{"content"});
    }

    TEST_F(UploadTests, CancelledWriteAbortsTheUpload)
    {
        // No plan begins, nothing reads the data, so the write waits for the cancel.
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}, "150 Ok to send data.")
            .abortSequence(AbortReplies::ProftpdPlainLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const TransferSession session{.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()};
        const auto written =
            run(connection,
                [connection, session]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    auto started = co_await Upload::start(session, {.path = "file"}, quickTransferOptions());
                    if (!started)
                        co_return std::unexpected(std::move(started).error());
                    const std::string content(32u << 20u, 'c');
                    const auto [exception, result] = co_await asio::co_spawn(
                        connection->strand(),
                        (*started)->write(asio::buffer(content)),
                        asio::cancel_after(300ms, asio::as_tuple(asio::use_awaitable))
                    );
                    if (exception)
                        std::rethrow_exception(exception);
                    co_return result;
                });
        ASSERT_FALSE(written);
        EXPECT_EQ(written.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << written.error().toString();
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"EPSV", "STOR file", "ABOR", "PWD"}));
    }

    TEST_F(UploadTests, DroppedUploadResetsItsData)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.expectUpload(1024u), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("426 Connection closed; transfer aborted.")
            .checkpoint("refused")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const TransferSession session{.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()};
        const auto written =
            run(connection,
                [session]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    auto started = co_await Upload::start(session, {.path = "file"}, quickTransferOptions());
                    if (!started)
                        co_return std::unexpected(std::move(started).error());
                    co_return co_await (*started)->write(asio::buffer(std::string_view{"partial"}));
                });
        ASSERT_TRUE(written) << written.error().toString();
        ASSERT_TRUE(server_->waitForCheckpoint("refused"));
        connection->close();
        EXPECT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(
            detailOf(server_->transcript(), TranscriptEntry::Kind::DataEnded), std::optional<std::string>{"reset"}
        );
    }

    TEST_F(UploadTests, PathThatCannotBeSentSendsNoRest)
    {
        Script script{};
        script.reply("220 Ready").expect("NOOP").reply("200 Fine").expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = upload(connection, {.path = "a\nb", .offset = 5u}, "x");
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().wrapperError, RemoteIo::WrapperErrors::InvalidPath) << result.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(UploadTests, ResetDataAbortsTheTransfer)
    {
        Script script{};
        script.reply("220 Ready");
        storeWith(script, DataPlan{}.reset(), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .abortSequence(AbortReplies::ProftpdPlainLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectUploadError(
            upload(connection, {.path = "file"}, std::string(1u << 20u, 'x')), RemoteIo::RemoteErrorKind::ConnectionLost
        );
    }

    TEST_F(UploadTests, TlsUploadResumesTheControlSession)
    {
        auto script = securedPrefix();
        storeWith(script, DataPlan{}.tls({.requireResumption = true}).expectUpload(1u << 20u), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const std::string content(200u * 1024u, 't');
        const auto result = upload(connection, {.path = "file"}, content, tls);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(uploadedAfterDisconnect(connection), content);
    }

    TEST_F(UploadTests, TlsUploadWhoseServerEndsWithoutCloseNotifyIsComplete)
    {
        auto script = securedPrefix();
        storeWith(script, DataPlan{}.tls({.closeWithoutCloseNotify = true}).expectUpload(1024u), "150 Ok to send data.")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const auto result = upload(connection, {.path = "file"}, "content", tls);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(uploadedAfterDisconnect(connection), std::optional<std::string>{"content"});
    }
}
