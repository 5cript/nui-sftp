#pragma once

#ifndef _WIN32

#    include "test_login_conformance.hpp"
#    include "test_upload.hpp"

#    include <ftp/client/transfer.hpp>

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
        auto started = onStrand(
            connection_,
            [session = session_, path = pathOf("large.bin"), size = whole.size(), options]()
                -> boost::asio::awaitable<std::expected<std::shared_ptr<Download>, RemoteIo::RemoteError>>
            {
                co_return co_await Download::start(session, {.path = path, .expectedSize = size}, options);
            }
        );
        ASSERT_TRUE(started) << started.error().toString();
        auto const download = std::move(started).value();
        const auto read = [this, &download](std::size_t size)
        {
            return onStrand(
                connection_,
                [download, size]() -> boost::asio::awaitable<DownloadResult>
                {
                    co_return co_await readPart(download, size);
                }
            );
        };
        const auto first = read(64u * 1024u);
        ASSERT_TRUE(first) << first.error().toString();
        // Nobody reads now. A command is written only once the transfer ended, so the reply to this one shows that
        // the drop aborted the RETR. It is through before the next read sends the commands that continue.
        const auto noop = onStrand(
            connection_,
            [connection = connection_]() -> boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>>
            {
                co_return co_await connection->execute(Command{.verb = Verb::Noop});
            }
        );
        ASSERT_TRUE(noop) << noop.error().toString();
        EXPECT_EQ(noop->code, 200);
        const auto rest = read(whole.size());
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(first->content.size() + rest->content.size(), whole.size());
        EXPECT_TRUE(first->content + rest->content == whole);
        const auto finished = onStrand(
            connection_,
            [download]() -> boost::asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>>
            {
                co_return co_await download->finish();
            }
        );
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, whole.size());
        ASSERT_TRUE(finished->finalReply);
        EXPECT_EQ(finished->finalReply->code / 100, 2);
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
