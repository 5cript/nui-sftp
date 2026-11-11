#pragma once

#include "test_listing_transfer.hpp"

#include <ftp/client/metadata.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        constexpr std::string_view listedFile = "-rw-r--r--    1 0        0               5 Jan 02 03:04 a\r\n";
        constexpr std::string_view listedHiddenFile =
            "-rw-r--r--    1 0        0               7 Jan 02 03:04 .hidden\r\n";

        /**
         * @brief EPSV, then @p verb with an argument matching @p argument whose data is @p data and whose final reply
         *        is @p finalReply.
         */
        Script& listingOf(
            Script& script,
            std::string verb,
            std::string argument,
            std::string data,
            std::string finalReply = "226 Directory send OK."
        )
        {
            return script.expect("EPSV")
                .passive(DataPlan{}.send(std::move(data)))
                .expect(std::move(verb), std::move(argument))
                .reply("150 Here comes the directory listing.")
                .beginData()
                .awaitData()
                .reply(std::move(finalReply));
        }

        /**
         * @brief EPSV, then @p verb refused at once with @p refusal.
         */
        Script& refusedListing(Script& script, std::string verb, std::string refusal)
        {
            return script.expect("EPSV").passive(DataPlan{}).expect(std::move(verb), "").reply(std::move(refusal));
        }

        Script& enters(Script& script, std::string directory)
        {
            return script.expect("CWD", std::move(directory)).reply("250 Directory successfully changed.");
        }
    }

    class MetadataTests : public ListingTransferTests
    {
      protected:
        MetadataSession sessionOf(std::shared_ptr<ControlConnection> const& connection) const
        {
            return MetadataSession{
                .transfer = {.control = connection, .passiveChoice = passiveChoice_},
                .dialect = dialect_,
                .options = quickTransferOptions(),
            };
        }

        ListingResult listIn(std::shared_ptr<ControlConnection> const& connection, std::string path)
        {
            return run(
                connection,
                [session = sessionOf(connection), path = std::move(path)]() -> asio::awaitable<ListingResult>
                {
                    co_return co_await listDirectory(session, path);
                }
            );
        }

        using EntryResult = std::expected<FileTransferProtocol::ListingEntry, RemoteIo::RemoteError>;

        EntryResult lstatIn(std::shared_ptr<ControlConnection> const& connection, std::string path)
        {
            return run(
                connection,
                [session = sessionOf(connection), path = std::move(path)]() -> asio::awaitable<EntryResult>
                {
                    co_return co_await lstatPath(session, path);
                }
            );
        }

        const std::shared_ptr<ServerDialect> dialect_ = std::make_shared<ServerDialect>();
        const std::shared_ptr<PassiveChoice> passiveChoice_ = std::make_shared<PassiveChoice>();
    };

    TEST_F(MetadataTests, TheListOptionIsProbedOnceAndKept)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/one"), "LIST", "", std::string{listedFile});
        listingOf(script, "LIST", "-a", std::string{listedHiddenFile} + std::string{listedFile});
        listingOf(enters(script, "/two"), "LIST", "-a", std::string{listedFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = listIn(connection, "/one");
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(listedNames(*first), (std::vector<std::string>{".hidden", "a"}));
        const auto second = listIn(connection, "/two");
        ASSERT_TRUE(second) << second.error().toString();
        EXPECT_EQ(listedNames(*second), (std::vector<std::string>{"a"}));
        EXPECT_EQ(dialect_->listAll(), Support::Works);
        EXPECT_EQ(dialect_->machineListing(), Support::Unknown);
    }

    TEST_F(MetadataTests, AListOptionTheServerTakesForAPathIsNotUsedAgain)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/one"), "LIST", "", std::string{listedFile});
        script.expect("EPSV").passive(DataPlan{}).expect("LIST", "-a").reply("550 -a: No such file or directory");
        listingOf(enters(script, "/two"), "LIST", "", std::string{listedFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = listIn(connection, "/one");
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(listedNames(*first), (std::vector<std::string>{"a"}));
        const auto second = listIn(connection, "/two");
        ASSERT_TRUE(second) << second.error().toString();
        EXPECT_EQ(dialect_->listAll(), Support::Refused);
    }

    TEST_F(MetadataTests, AListOptionThatListsLessIsNotUsed)
    {
        // A server that lists the path "-a" as a missing directory, empty.
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/one"), "LIST", "", std::string{listedFile});
        listingOf(script, "LIST", "-a", "");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/one");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(listedNames(*listed), (std::vector<std::string>{"a"}));
        EXPECT_EQ(dialect_->listAll(), Support::Refused);
    }

    TEST_F(MetadataTests, AnEmptyDirectoryDoesNotDecideTheListOption)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/empty"), "LIST", "", "");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/empty");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_TRUE(listed->entries.empty());
        EXPECT_EQ(dialect_->listAll(), Support::Unknown);
    }

    TEST_F(MetadataTests, ADirectoryThatCannotBeEnteredIsNotListed)
    {
        // vsftpd lists such a directory as empty, the CWD tells.
        Script script{};
        script.reply("220 Ready")
            .expect("CWD", "/missing")
            .reply("550 Failed to change directory.")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/missing");
        ASSERT_FALSE(listed);
        EXPECT_EQ(listed.error().protocolCode, 550);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(MetadataTests, AListThatCannotBeReadIsListedAgainWithMlsd)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/odd"), "LIST", "", "an entry in no format the parser reads\r\n");
        listingOf(script, "MLSD", "", "type=file;size=1; a\r\n");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/odd");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(listedNames(*listed), (std::vector<std::string>{"a"}));
        EXPECT_EQ(listed->unreadableLines, 0u);
        EXPECT_EQ(dialect_->machineListing(), Support::Works);
    }

    TEST_F(MetadataTests, AListThatCannotBeReadStaysWhereMlsdIsMissing)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/odd"), "LIST", "", "an entry in no format the parser reads\r\n");
        refusedListing(script, "MLSD", "500 Unknown command.");
        listingOf(enters(script, "/odd"), "LIST", "", "an entry in no format the parser reads\r\n");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = listIn(connection, "/odd");
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(first->unreadableLines, 1u);
        EXPECT_EQ(dialect_->machineListing(), Support::Refused);
        const auto second = listIn(connection, "/odd");
        ASSERT_TRUE(second) << second.error().toString();
        EXPECT_EQ(second->unreadableLines, 1u);
    }

    TEST_F(MetadataTests, AnMlsdRefusedForOneDirectoryKeepsItsList)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/odd"), "LIST", "", "an entry in no format the parser reads\r\n");
        refusedListing(script, "MLSD", "550 /odd: Permission denied");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/odd");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(listed->unreadableLines, 1u);
        EXPECT_EQ(dialect_->machineListing(), Support::Unknown);
    }

    TEST_F(MetadataTests, AStatTheServerLacksIsLearnedAndTheParentLists)
    {
        Script script{};
        script.reply("220 Ready").expect("STAT", "/dir/a").reply("502 Command not implemented.");
        listingOf(enters(script, "/dir"), "LIST", "", std::string{listedFile});
        listingOf(script, "LIST", "-a", std::string{listedFile});
        listingOf(enters(script, "/dir"), "LIST", "-a", std::string{listedFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = lstatIn(connection, "/dir/a");
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(first->name, "a");
        EXPECT_EQ(first->size, std::optional<std::uint64_t>{5u});
        EXPECT_EQ(dialect_->status(), Support::Refused);
        const auto second = lstatIn(connection, "/dir/a");
        ASSERT_TRUE(second) << second.error().toString();
    }

    TEST_F(MetadataTests, BelowADirectoryThatCannotBeEnteredIsPermissionDenied)
    {
        // vsftpd's refusals say nothing, the directory's own entry tells that it exists.
        Script script{};
        script.reply("220 Ready")
            .expect("STAT", "/denied/a")
            .replyRaw("213-Status follows:\r\n213 End of status\r\n")
            .expect("CWD", "/denied")
            .reply("550 Failed to change directory.")
            .expect("STAT", "/denied")
            .replyRaw("213-Status follows:\r\n213 End of status\r\n");
        listingOf(
            enters(script, "/"), "LIST", "", "drwx------    2 0        0            4096 Jan 02 03:04 denied\r\n"
        );
        listingOf(script, "LIST", "-a", "drwx------    2 0        0            4096 Jan 02 03:04 denied\r\n");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto denied = lstatIn(connection, "/denied/a");
        ASSERT_FALSE(denied);
        EXPECT_EQ(denied.error().kind, RemoteIo::RemoteErrorKind::PermissionDenied) << denied.error().toString();
    }

    TEST(DirectoryEntryConversionTests, ANameThatIsNoUtf8IsShownAndMarked)
    {
        const auto converted = toDirectoryEntry(
            FileTransferProtocol::ListingEntry{.name = "a\xff.txt", .type = SharedData::FileType::Regular}
        );
        EXPECT_EQ(converted.name, "a\xef\xbf\xbd.txt");
        EXPECT_FALSE(converted.nameIsValidUtf8);
        EXPECT_TRUE(toDirectoryEntry(FileTransferProtocol::ListingEntry{.name = "\xc3\xbc.txt"}).nameIsValidUtf8);
    }

    TEST(DirectoryEntryConversionTests, AMachineListedTimeKeepsItsFraction)
    {
        using namespace std::chrono;
        const auto modified = TimeVal{sys_days{2026y / January / 2} + 3h + 4min + 5s + 250ms};
        const auto converted =
            toDirectoryEntry(FileTransferProtocol::ListingEntry{.name = "a", .size = 3u, .modified = modified});
        EXPECT_EQ(converted.mtime, static_cast<std::uint64_t>(floor<seconds>(modified).time_since_epoch().count()));
        EXPECT_EQ(converted.mtimeNsec, 250'000'000u);
        EXPECT_EQ(converted.size, 3u);
    }

    TEST(DirectoryEntryConversionTests, AListedTimeIsReadAsUtc)
    {
        using namespace std::chrono;
        const auto listed = local_days{2026y / January / 2} + 3h + 4min;
        const auto converted = toDirectoryEntry(
            FileTransferProtocol::ListingEntry{
                .name = "a", .listedTime = ListedTime{.time = listed, .precision = ListedTimePrecision::Minute}
            }
        );
        EXPECT_EQ(
            converted.mtime,
            static_cast<std::uint64_t>(
                sys_seconds{sys_days{2026y / January / 2} + 3h + 4min}.time_since_epoch().count()
            )
        );
        EXPECT_EQ(converted.mtimeNsec, 0u);
    }

    TEST(DirectoryEntryConversionTests, ALinkKeepsItsTargetAsUtf8)
    {
        const auto converted = toDirectoryEntry(
            FileTransferProtocol::ListingEntry{
                .name = "link", .type = SharedData::FileType::Symlink, .linkTarget = std::string{"t\xff"}
            }
        );
        EXPECT_EQ(converted.type, SharedData::FileType::Symlink);
        EXPECT_EQ(converted.linkTarget, std::optional<std::string>{"t\xef\xbf\xbd"});
    }
}
