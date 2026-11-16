#pragma once

#ifndef _WIN32

#    include "test_login_conformance.hpp"
#    include "test_upload.hpp"

#    include <ftp/client/transfer.hpp>
#    include <ftp/protocol/reply_parsers.hpp>

#    include <gtest/gtest.h>

#    include <algorithm>
#    include <array>
#    include <memory>
#    include <string>
#    include <string_view>
#    include <utility>
#    include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /**
         * @brief What servers/common/fixtures.sh writes into a fixture of @p size bytes.
         */
        std::string fixtureContent(std::size_t size)
        {
            constexpr std::string_view unit = "nui-sftp\n";
            std::string content{};
            content.reserve(size);
            while (content.size() < size)
                content.append(unit.substr(0u, std::min(unit.size(), size - content.size())));
            return content;
        }

        /**
         * @brief Bytes of every value, so a text mode or a dropped byte would show.
         */
        std::string uploadContent(std::size_t size)
        {
            std::string content(size, '\0');
            for (std::size_t index = 0u; index < size; ++index)
                content[index] = static_cast<char>((index * 131u + 7u) % 256u);
            return content;
        }

        struct Fixture
        {
            std::string_view name{};
            std::size_t size{0u};
        };

        constexpr std::array<Fixture, 5> sizedFixtures{{
            {"empty.bin", 0u},
            {"one.bin", 1u},
            {"almost-a-chunk.bin", 16383u},
            {"chunk.bin", 16384u},
            {"large.bin", 10485760u},
        }};
    }

    /**
     * @brief Transfers against every selected server, plain and over both kinds of TLS. Every case works in a copy
     *        of the fixtures of its own, and all transfers of a case share one login, so a TLS server that requires
     *        session reuse sees each data connection resume.
     */
    class TransferConformance : public LoginConformance
    {
      protected:
        void SetUp() override
        {
            LoginConformance::SetUp();
            if (IsSkipped() || HasFatalFailure())
                return;
            auto workspace = prepareWorkspace(endpoint_);
            ASSERT_TRUE(workspace.has_value()) << workspace.error();
            workspace_ = std::move(workspace).value();
            ASSERT_NO_FATAL_FAILURE(logInOnce());
        }

        /**
         * @brief A login whose certificate, for TLS, was pinned after a first refused attempt.
         */
        void logInOnce()
        {
            auto loginOptions = options();
            if (mode() != TlsMode::None)
            {
                const auto probe = makeConnection();
                const auto refused = login(probe, options());
                ASSERT_FALSE(refused);
                ASSERT_TRUE(probe->untrustedCertificate());
                loginOptions = options(probe->untrustedCertificate()->publicKeySha256);
            }
            connection_ = makeConnection();
            const auto result = login(connection_, loginOptions);
            ASSERT_TRUE(result) << result.error().toString();
            session_ = TransferSession{
                .control = connection_,
                .passiveChoice = std::make_shared<PassiveChoice>(),
                .tls = loginOptions.tls,
                .restartStream = result->features.has("REST", "STREAM"),
            };
            features_ = result->features;
        }

        std::string pathOf(std::string_view name) const
        {
            return fmt::format("{}/{}", workspace_, name);
        }

        DownloadResult download(DownloadRequest request)
        {
            return onStrand(
                connection_,
                [session = session_, request = std::move(request)]() -> boost::asio::awaitable<DownloadResult>
                {
                    co_return co_await downloadAll(session, request, transferOptions());
                }
            );
        }

        UploadOutcome upload(UploadRequest request, std::string content)
        {
            return onStrand(
                connection_,
                [session = session_,
                    request = std::move(request),
                    content = std::move(content)]() -> boost::asio::awaitable<UploadOutcome>
                {
                    co_return co_await uploadAll(session, request, content, transferOptions(), 256u * 1024u);
                }
            );
        }

        std::expected<std::shared_ptr<Download>, RemoteIo::RemoteError>
        startDownload(DownloadRequest request, TransferOptions options = transferOptions())
        {
            return onStrand(
                connection_,
                [session = session_, request = std::move(request), options]()
                    -> boost::asio::awaitable<std::expected<std::shared_ptr<Download>, RemoteIo::RemoteError>>
                {
                    co_return co_await Download::start(session, request, options);
                }
            );
        }

        /**
         * @brief Reads until @p size bytes came or the data ended.
         */
        DownloadResult readFrom(std::shared_ptr<Download> const& download, std::size_t size)
        {
            return onStrand(
                connection_,
                [download, size]() -> boost::asio::awaitable<DownloadResult>
                {
                    co_return co_await readPart(download, size);
                }
            );
        }

        std::expected<TransferResult, RemoteIo::RemoteError> finishDownload(std::shared_ptr<Download> const& download)
        {
            return onStrand(
                connection_,
                [download]() -> boost::asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>>
                {
                    co_return co_await download->finish();
                }
            );
        }

        std::expected<Reply, RemoteIo::RemoteError> execute(Command command)
        {
            return onStrand(
                connection_,
                [connection = connection_, command = std::move(command)]()
                    -> boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>>
                {
                    co_return co_await connection->execute(command);
                }
            );
        }

        /**
         * @brief A command behind an abort waits for its drain, so its reply shows that the login stayed usable.
         */
        void expectUsableLogin()
        {
            const auto noop = execute(Command{.verb = Verb::Noop});
            ASSERT_TRUE(noop) << noop.error().toString();
            EXPECT_EQ(noop->code, 200);
        }

        static TransferOptions transferOptions()
        {
            return TransferOptions{
                .data =
                    DataConnectionOptions{.connectTimeout = 10s, .inactivityTimeout = 10s, .closeNotifyTimeout = 5s},
                .finalReplyTimeout = 10s,
            };
        }

        void expectDownload(DownloadRequest request, std::string_view content)
        {
            const auto result = download(request);
            ASSERT_TRUE(result) << request.path << ": " << result.error().toString();
            EXPECT_EQ(result->content.size(), content.size()) << request.path;
            EXPECT_TRUE(result->content == content) << request.path;
            ASSERT_TRUE(result->result.finalReply) << request.path;
            EXPECT_EQ(result->result.finalReply->code / 100, 2) << request.path;
        }

      protected:
        std::string workspace_{};
        std::shared_ptr<ControlConnection> connection_{};
        TransferSession session_{};
        /** @brief What FEAT listed at the login. */
        Features features_{};
    };

    TEST_P(TransferConformance, DownloadsEveryFixtureSize)
    {
        for (auto const& fixture : sizedFixtures)
        {
            ASSERT_NO_FATAL_FAILURE(expectDownload(
                {.path = pathOf(fixture.name), .expectedSize = fixture.size}, fixtureContent(fixture.size)
            ));
        }
    }

    TEST_P(TransferConformance, DownloadsWithoutAKnownSize)
    {
        ASSERT_NO_FATAL_FAILURE(expectDownload({.path = pathOf("chunk.bin")}, fixtureContent(16384u)));
    }

    TEST_P(TransferConformance, ResumesADownload)
    {
        const auto whole = fixtureContent(16384u);
        ASSERT_NO_FATAL_FAILURE(expectDownload(
            {.path = pathOf("chunk.bin"), .offset = 1000u, .expectedSize = whole.size()},
            std::string_view{whole}.substr(1000u)
        ));
    }

    TEST_P(TransferConformance, PausedDownloadContinuesWithRest)
    {
        ASSERT_TRUE(session_.restartStream) << "the server does not list REST STREAM";
        const auto whole = fixtureContent(10485760u);
        auto options = transferOptions();
        options.pauseDropAfter = 300ms;
        auto started = startDownload({.path = pathOf("large.bin"), .expectedSize = whole.size()}, options);
        ASSERT_TRUE(started) << started.error().toString();
        auto const download = std::move(started).value();
        const auto first = readFrom(download, 64u * 1024u);
        ASSERT_TRUE(first) << first.error().toString();
        // Nobody reads now. A command is written only once the transfer ended, so the reply to this one shows that
        // the drop aborted the RETR. It is through before the next read sends the commands that continue.
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
        const auto rest = readFrom(download, whole.size());
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(first->content.size() + rest->content.size(), whole.size());
        EXPECT_TRUE(first->content + rest->content == whole);
        const auto finished = finishDownload(download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, whole.size());
        ASSERT_TRUE(finished->finalReply);
        EXPECT_EQ(finished->finalReply->code / 100, 2);
    }

    TEST_P(TransferConformance, AbortedDownloadKeepsTheLogin)
    {
        auto started = startDownload({.path = pathOf("large.bin"), .expectedSize = 10485760u});
        ASSERT_TRUE(started) << started.error().toString();
        auto const download = std::move(started).value();
        const auto first = readFrom(download, 64u * 1024u);
        ASSERT_TRUE(first) << first.error().toString();
        onStrand(
            connection_,
            [download]() -> boost::asio::awaitable<void>
            {
                download->abort();
                co_return;
            }
        );
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
        ASSERT_NO_FATAL_FAILURE(expectDownload({.path = pathOf("one.bin"), .expectedSize = 1u}, fixtureContent(1u)));
    }

    TEST_P(TransferConformance, AnAbortBeforeTheFirstByteOrAfterTheLastKeepsTheLogin)
    {
        const auto whole = fixtureContent(16384u);
        for (const auto readFirst : {false, true})
        {
            auto started = startDownload({.path = pathOf("chunk.bin"), .expectedSize = whole.size()});
            ASSERT_TRUE(started) << started.error().toString();
            auto const download = std::move(started).value();
            if (readFirst)
            {
                // One byte more than the file has, so the read sees the data end.
                const auto all = readFrom(download, whole.size() + 1u);
                ASSERT_TRUE(all) << all.error().toString();
                EXPECT_EQ(all->content.size(), whole.size());
            }
            onStrand(
                connection_,
                [download]() -> boost::asio::awaitable<void>
                {
                    download->abort();
                    co_return;
                }
            );
            ASSERT_NO_FATAL_FAILURE(expectUsableLogin()) << "read first: " << readFirst;
            ASSERT_NO_FATAL_FAILURE(expectDownload({.path = pathOf("one.bin"), .expectedSize = 1u}, fixtureContent(1u)))
                << "read first: " << readFirst;
        }
    }

    TEST_P(TransferConformance, EveryDataConnectionOfALoginResumesItsTlsSession)
    {
        const auto resumes = mode() != TlsMode::None && profile_.expectations.resumesDataSessions;
        // Six data connections of one control connection, uploads and downloads in turn.
        for (std::size_t round = 0u; round < 3u; ++round)
        {
            const auto name = fmt::format("resumed-{}.bin", round);
            const auto content = uploadContent(1000u + round);
            const auto uploaded = upload({.path = pathOf(name)}, content);
            ASSERT_TRUE(uploaded) << name << ": " << uploaded.error().toString();
            EXPECT_EQ(uploaded->resumedSession, resumes) << name;
            const auto downloaded = download({.path = pathOf(name), .expectedSize = content.size()});
            ASSERT_TRUE(downloaded) << name << ": " << downloaded.error().toString();
            EXPECT_TRUE(downloaded->content == content) << name;
            EXPECT_EQ(downloaded->result.resumedSession, resumes) << name;
        }
    }

    TEST_P(TransferConformance, SeekBackContinuesAtThePosition)
    {
        const auto whole = fixtureContent(10485760u);
        auto started = startDownload({.path = pathOf("large.bin"), .expectedSize = whole.size()});
        ASSERT_TRUE(started) << started.error().toString();
        auto const download = std::move(started).value();
        const auto first = readFrom(download, 64u * 1024u);
        ASSERT_TRUE(first) << first.error().toString();
        const auto sought = onStrand(
            connection_,
            [download]() -> boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
            {
                co_return download->seek(1000u);
            }
        );
        ASSERT_TRUE(sought) << sought.error().toString();
        const auto rest = readFrom(download, whole.size());
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(rest->content.size(), whole.size() - 1000u);
        EXPECT_TRUE(rest->content == std::string_view{whole}.substr(1000u));
        const auto finished = finishDownload(download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, whole.size() - 1000u);
        ASSERT_TRUE(finished->finalReply);
        EXPECT_EQ(finished->finalReply->code / 100, 2);
    }

    TEST_P(TransferConformance, AbortedUploadIsNotCompleteAndContinuesWithRest)
    {
        const auto content = uploadContent(10485760u);
        const auto path = pathOf("aborted.bin");
        constexpr std::size_t written = 1u << 20u;
        const auto aborted = onStrand(
            connection_,
            [session = session_, path, content]() -> boost::asio::awaitable<UploadOutcome>
            {
                auto started = co_await Upload::start(session, {.path = path}, transferOptions());
                if (!started)
                    co_return std::unexpected(std::move(started).error());
                auto const upload = std::move(started).value();
                constexpr std::size_t chunkSize = 256u * 1024u;
                for (std::size_t offset = 0u; offset < written; offset += chunkSize)
                {
                    if (auto sent = co_await upload->write(asio::buffer(content.data() + offset, chunkSize)); !sent)
                        co_return std::unexpected(std::move(sent).error());
                }
                upload->abort();
                co_return co_await upload->finish();
            }
        );
        ASSERT_FALSE(aborted);
        EXPECT_EQ(aborted.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << aborted.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
        // Both servers keep the partial file, it holds at most what was written.
        const auto sizeReply = execute(Command{.verb = Verb::Size, .argument = path});
        ASSERT_TRUE(sizeReply) << sizeReply.error().toString();
        const auto kept = parseSizeReply(*sizeReply);
        ASSERT_TRUE(kept) << kept.error().toString();
        EXPECT_LE(*kept, written);
        const auto rest = upload({.path = path, .offset = *kept}, content.substr(*kept));
        ASSERT_TRUE(rest) << rest.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectDownload({.path = path, .expectedSize = content.size()}, content));
    }

    TEST_P(TransferConformance, AnUploadAbortedBeforeTheFirstByteOrAfterTheLastIsNotComplete)
    {
        const auto content = uploadContent(64u * 1024u);
        for (const auto writeAll : {false, true})
        {
            const auto path = pathOf(writeAll ? "aborted-at-the-end.bin" : "aborted-at-the-start.bin");
            const auto aborted = onStrand(
                connection_,
                [session = session_, path, content, writeAll]() -> boost::asio::awaitable<UploadOutcome>
                {
                    auto started = co_await Upload::start(session, {.path = path}, transferOptions());
                    if (!started)
                        co_return std::unexpected(std::move(started).error());
                    auto const upload = std::move(started).value();
                    if (writeAll)
                    {
                        if (auto sent = co_await upload->write(asio::buffer(content)); !sent)
                            co_return std::unexpected(std::move(sent).error());
                    }
                    upload->abort();
                    co_return co_await upload->finish();
                }
            );
            ASSERT_FALSE(aborted) << "write all: " << writeAll;
            EXPECT_EQ(aborted.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << aborted.error().toString();
            ASSERT_NO_FATAL_FAILURE(expectUsableLogin()) << "write all: " << writeAll;
            // What a server keeps of an upload that was cut behind its last byte is its own matter.
            if (!writeAll)
            {
                const auto sizeReply = execute(Command{.verb = Verb::Size, .argument = path});
                ASSERT_TRUE(sizeReply) << sizeReply.error().toString();
                // Nothing was sent: no file is as good as an empty one.
                EXPECT_EQ(parseSizeReply(*sizeReply).value_or(0u), 0u);
            }
        }
    }

    TEST_P(TransferConformance, UploadsReadBackTheSame)
    {
        for (
            const auto size :
            {std::size_t{0u}, std::size_t{1u}, std::size_t{16383u}, std::size_t{16384u}, std::size_t{10485760u}}
        )
        {
            const auto name = fmt::format("uploaded-{}.bin", size);
            const auto content = uploadContent(size);
            const auto uploaded = upload({.path = pathOf(name)}, content);
            ASSERT_TRUE(uploaded) << name << ": " << uploaded.error().toString();
            EXPECT_EQ(uploaded->finalReply.code / 100, 2) << name;
            ASSERT_NO_FATAL_FAILURE(expectDownload({.path = pathOf(name), .expectedSize = size}, content));
        }
    }

    TEST_P(TransferConformance, ContinuesAnUploadWithRest)
    {
        const auto content = uploadContent(100'000u);
        const auto path = pathOf("continued.bin");
        const auto first = upload({.path = path}, content.substr(0u, 40'000u));
        ASSERT_TRUE(first) << first.error().toString();
        const auto rest = upload({.path = path, .offset = 40'000u}, content.substr(40'000u));
        ASSERT_TRUE(rest) << rest.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectDownload({.path = path, .expectedSize = content.size()}, content));
    }

    TEST_P(TransferConformance, ContinuesAnUploadWithAppe)
    {
        const auto content = uploadContent(100'000u);
        const auto path = pathOf("appended.bin");
        const auto first = upload({.path = path}, content.substr(0u, 40'000u));
        ASSERT_TRUE(first) << first.error().toString();
        const auto rest = upload({.path = path, .mode = UploadMode::Append}, content.substr(40'000u));
        ASSERT_TRUE(rest) << rest.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectDownload({.path = path, .expectedSize = content.size()}, content));
    }

    TEST_P(TransferConformance, MissingFileFailsAndTheLoginStays)
    {
        const auto missing = download({.path = pathOf("does-not-exist.bin")});
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().protocolCode, 550) << missing.error().toString();
        // vsftpd sends one fixed text for every 550, only other servers say what is wrong.
        if (profile_.expectations.repliesTell550Causes)
            EXPECT_EQ(missing.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile) << missing.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectDownload({.path = pathOf("one.bin"), .expectedSize = 1u}, fixtureContent(1u)));
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        TransferConformance,
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
