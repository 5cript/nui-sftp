#pragma once

#include "test_download.hpp"

#include <ftp/client/transfer.hpp>
#include <ftp/protocol/listing.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using ListingResult = std::expected<Listing, RemoteIo::RemoteError>;

        constexpr std::string_view machineListingData =
            "type=cdir;modify=20260101120000; .\r\n"
            "type=file;size=3;modify=20260102030405; a.txt\r\n"
            "type=dir;modify=20260103000000; sub\r\n";

        ListingParser machineParser()
        {
            return ListingParser::machineListing();
        }

        std::vector<std::string> listedNames(Listing const& listing)
        {
            std::vector<std::string> names{};
            for (auto const& entry : listing.entries)
                names.push_back(entry.name);
            return names;
        }
    }

    class ListingTransferTests : public DownloadTlsTests
    {
      protected:
        ListingResult listWith(
            std::shared_ptr<ControlConnection> const& connection,
            Command command,
            ListingParser parser,
            std::optional<TlsConnectionSettings> tls = std::nullopt,
            TransferOptions options = quickTransferOptions()
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
                    command = std::move(command),
                    parser = std::move(parser),
                    options]() mutable -> asio::awaitable<ListingResult>
                {
                    co_return co_await fetchListing(session, std::move(command), std::move(parser), options);
                }
            );
        }

        static Command machineListing()
        {
            return Command{.verb = Verb::MachineListDirectory};
        }

        static void expectListingError(ListingResult const& result, RemoteIo::RemoteErrorKind kind)
        {
            ASSERT_FALSE(result) << "listed " << result->entries.size() << " entries";
            EXPECT_EQ(result.error().kind, kind) << result.error().toString();
        }
    };

    TEST_F(ListingTransferTests, ListingDataInPiecesBecomesTheListing)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{}.send(std::string{machineListingData}, 7u))
            .expect("MLSD", "")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = listWith(connection, machineListing(), machineParser());
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(listedNames(*result), (std::vector<std::string>{"a.txt", "sub"}));
        EXPECT_EQ(result->entries.front().size, std::optional<std::uint64_t>{3u});
        EXPECT_EQ(result->entries.back().type, SharedData::FileType::Directory);
    }

    TEST_F(ListingTransferTests, TheArgumentOfTheListingCommandGoesOut)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{}.send("-rw-r--r--    1 0        0               5 Jan 02 03:04 a.txt\r\n"))
            .expect("LIST", "-a")
            .reply("150 Here comes the directory listing.")
            .beginData()
            .awaitData()
            .reply("226 Directory send OK.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = listWith(
            connection,
            Command{.verb = Verb::List, .argument = "-a"},
            ListingParser::list(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()))
        );
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(listedNames(*result), (std::vector<std::string>{"a.txt"}));
    }

    TEST_F(ListingTransferTests, ANegativeFinalReplyIsTheErrorAndKeepsTheLogin)
    {
        // proftpd's answer for a directory that does not exist.
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{})
            .expect("MLSD", "missing")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .awaitData()
            .reply("450 missing: No such file or directory")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectListingError(
            listWith(connection, Command{.verb = Verb::MachineListDirectory, .argument = "missing"}, machineParser()),
            RemoteIo::RemoteErrorKind::NoSuchFile
        );
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ListingTransferTests, ARefusedListingKeepsTheLogin)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{})
            .expect("MLSD")
            .reply("550 Permission denied")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectListingError(
            listWith(connection, machineListing(), machineParser()), RemoteIo::RemoteErrorKind::PermissionDenied
        );
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ListingTransferTests, SilentDataAbortsTheListingAndKeepsTheLogin)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{}.stallAfter("type=file; a"))
            .expect("MLSD")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.data.inactivityTimeout = 300ms;
        expectListingError(
            listWith(connection, machineListing(), machineParser(), std::nullopt, options),
            RemoteIo::RemoteErrorKind::TimedOut
        );
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"EPSV", "MLSD", "ABOR", "PWD"}));
    }

    TEST_F(ListingTransferTests, AMissingFinalReplyIsTimedOut)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{}.send(std::string{machineListingData}))
            .expect("MLSD")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .awaitData()
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.finalReplyTimeout = 300ms;
        expectListingError(
            listWith(connection, machineListing(), machineParser(), std::nullopt, options),
            RemoteIo::RemoteErrorKind::TimedOut
        );
    }

    TEST_F(ListingTransferTests, ATlsListingResumesTheControlSession)
    {
        auto script = securedPrefix();
        script.expect("EPSV")
            .passive(DataPlan{}.tls({.requireResumption = true}).send(std::string{machineListingData}))
            .expect("MLSD")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const auto result = listWith(connection, machineListing(), machineParser(), tls);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(listedNames(*result), (std::vector<std::string>{"a.txt", "sub"}));
    }

    TEST_F(ListingTransferTests, ATlsListingCancelledBeforeItsFirstByteClosesWithTheServersAnswer)
    {
        // Nothing comes on the data connection, the read waits for the cancel. The server ends the data for the ABOR.
        auto script = securedPrefix();
        script.expect("EPSV")
            .passive(DataPlan{}.tls().stallAfter(""))
            .expect("MLSD")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .abortSequence(AbortReplies::ProftpdTlsLike)
            .stopData()
            .expect("PWD")
            .reply(currentDirectory)
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const TransferSession session{
            .control = connection,
            .passiveChoice = std::make_shared<PassiveChoice>(),
            .tls = tls,
        };
        const auto listed =
            run(connection,
                [connection, session]() -> asio::awaitable<ListingResult>
                {
                    auto [exception, result] = co_await asio::co_spawn(
                        connection->strand(),
                        fetchListing(session, machineListing(), machineParser(), quickTransferOptions()),
                        asio::cancel_after(300ms, asio::as_tuple(asio::use_awaitable))
                    );
                    if (exception)
                        std::rethrow_exception(exception);
                    co_return result;
                });
        expectListingError(listed, RemoteIo::RemoteErrorKind::Cancelled);
        expectReply(execute(connection, command(Verb::Noop)), 200);
        EXPECT_EQ(dataEndAfterDisconnect(connection), std::nullopt);
    }

    TEST_F(ListingTransferTests, TlsListingDataWithoutCloseNotifyIsTruncated)
    {
        auto script = securedPrefix();
        script.expect("EPSV")
            .passive(DataPlan{}.tls({.closeWithoutCloseNotify = true}).send(std::string{machineListingData}))
            .expect("MLSD")
            .reply("150 Opening ASCII mode data connection for MLSD")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        expectListingError(
            listWith(connection, machineListing(), machineParser(), tls), RemoteIo::RemoteErrorKind::Truncated
        );
    }
}
