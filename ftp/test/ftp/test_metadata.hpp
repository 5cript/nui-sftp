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

        constexpr std::string_view statusOfARootWithDots =
            "213-Status follows:\r\n"
            "drwxr-xr-x    3 0        0            4096 Jan 02 03:04 .\r\n"
            "drwxr-xr-x    3 0        0            4096 Jan 02 03:04 ..\r\n"
            "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 dir\r\n"
            "213 End of status\r\n";

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

        EntryResult probeIn(std::shared_ptr<ControlConnection> const& connection, std::string path)
        {
            return run(
                connection,
                [session = sessionOf(connection), path = std::move(path)]() -> asio::awaitable<EntryResult>
                {
                    co_return co_await probePath(session, path);
                }
            );
        }

        std::expected<void, RemoteIo::RemoteError>
        setTimeIn(std::shared_ptr<ControlConnection> const& connection, std::string path, std::chrono::sys_seconds time)
        {
            return run(
                connection,
                [session = sessionOf(connection),
                    path = std::move(path),
                    time]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    co_return co_await setModificationTime(session, path, time);
                }
            );
        }

        /**
         * @brief Cancels @p operation once the server reached the checkpoint @p received, then lets the server go on
         *        with @p answer.
         */
        template <typename ResultT>
        ResultT cancelledAt(
            std::shared_ptr<ControlConnection> const& connection,
            asio::awaitable<ResultT> operation,
            std::string const& received,
            std::string answer
        )
        {
            const auto signal = std::make_shared<asio::cancellation_signal>();
            auto running = asio::co_spawn(
                connection->strand(), std::move(operation), asio::bind_cancellation_slot(signal->slot(), asio::use_future)
            );
            if (!server_->waitForCheckpoint(received))
                throw std::runtime_error{"The server did not reach " + received};
            asio::post(
                connection->strand(),
                [signal]()
                {
                    signal->emit(asio::cancellation_type::terminal);
                }
            );
            if (running.wait_for(20s) != std::future_status::ready)
                throw std::runtime_error{"The cancelled operation did not end"};
            server_->release(std::move(answer));
            return running.get();
        }

        std::expected<void, RemoteIo::RemoteError>
        removeDirectoryIn(std::shared_ptr<ControlConnection> const& connection, std::string path)
        {
            return run(
                connection,
                [session = sessionOf(connection),
                    path = std::move(path)]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    co_return co_await removeDirectory(session, path);
                }
            );
        }

        static std::chrono::sys_seconds someTime()
        {
            using namespace std::chrono;
            return sys_seconds{sys_days{2020y / January / 2} + 3h + 4min + 5s};
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
        listingOf(script, "LIST", "-a", "");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/empty");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_TRUE(listed->entries.empty());
        EXPECT_EQ(dialect_->listAll(), Support::Unknown);
    }

    TEST_F(MetadataTests, ADirectoryOfHiddenEntriesAloneIsListedWithThem)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/home"), "LIST", "", "");
        listingOf(script, "LIST", "-a", std::string{listedHiddenFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/home");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(listedNames(*listed), (std::vector<std::string>{".hidden"}));
        EXPECT_EQ(dialect_->listAll(), Support::Works);
    }

    TEST_F(MetadataTests, ARefusedDataConnectionDoesNotDecideTheListOption)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/one"), "LIST", "", std::string{listedFile});
        script.expect("EPSV").passive(DataPlan{}).expect("LIST", "-a").reply("425 Can't open data connection.");
        listingOf(enters(script, "/one"), "LIST", "", std::string{listedFile});
        listingOf(script, "LIST", "-a", std::string{listedHiddenFile} + std::string{listedFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = listIn(connection, "/one");
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(listedNames(*first), (std::vector<std::string>{"a"}));
        EXPECT_EQ(dialect_->listAll(), Support::Unknown);
        const auto second = listIn(connection, "/one");
        ASSERT_TRUE(second) << second.error().toString();
        EXPECT_EQ(listedNames(*second), (std::vector<std::string>{".hidden", "a"}));
        EXPECT_EQ(dialect_->listAll(), Support::Works);
    }

    TEST_F(MetadataTests, ADirectoryThatChangedBetweenTheListingsDoesNotDecideTheListOption)
    {
        // Another client removed b in between.
        Script script{};
        script.reply("220 Ready");
        listingOf(
            enters(script, "/busy"),
            "LIST",
            "",
            std::string{listedFile} + "-rw-r--r--    1 0        0               5 Jan 02 03:04 b\r\n"
        );
        listingOf(script, "LIST", "-a", std::string{listedHiddenFile} + std::string{listedFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/busy");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(listedNames(*listed), (std::vector<std::string>{"a", "b"}));
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

    TEST_F(MetadataTests, MlsdAddsWhatAListLeftOutAndTheListKeepsItsLinks)
    {
        // MLSD names no target and, on proftpd, no dangling link.
        const std::string listed =
            "lrwxrwxrwx    1 0        0               6 Jan 02 03:04 link -> target\r\n"
            "lrwxrwxrwx    1 0        0               7 Jan 02 03:04 dangling -> nowhere\r\n"
            "an entry in no format the parser reads\r\n";
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/odd"), "LIST", "", listed);
        listingOf(script, "LIST", "-a", listed);
        listingOf(script, "MLSD", "", "type=file;size=6; link\r\ntype=file;size=1; odd\r\n");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto merged = listIn(connection, "/odd");
        ASSERT_TRUE(merged) << merged.error().toString();
        EXPECT_EQ(listedNames(*merged), (std::vector<std::string>{"link", "dangling", "odd"}));
        EXPECT_EQ(merged->entries.front().type, SharedData::FileType::Symlink);
        EXPECT_EQ(merged->entries.front().linkTarget, std::optional<std::string>{"target"});
        EXPECT_EQ(merged->unreadableLines, 0u);
    }

    TEST_F(MetadataTests, AnMlsdThatCannotBeReadEitherKeepsTheList)
    {
        const std::string listed = std::string{listedFile} + "an entry in no format the parser reads\r\n";
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/odd"), "LIST", "", listed);
        listingOf(script, "LIST", "-a", listed);
        listingOf(script, "MLSD", "", "no facts\r\nnone here\r\n");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto kept = listIn(connection, "/odd");
        ASSERT_TRUE(kept) << kept.error().toString();
        EXPECT_EQ(listedNames(*kept), (std::vector<std::string>{"a"}));
        EXPECT_EQ(kept->unreadableLines, 1u);
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

    TEST_F(MetadataTests, LstatReadsTheStatusLinesOfVsftpdAndProftpdWithoutAListing)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("STAT", "/dir/plain.txt")
            .replyRaw(
                "213-Status follows:\r\n"
                "-rw-r--r--    1 1000     1000           11 Jan 02 03:04 plain.txt\r\n"
                "213 End of status\r\n"
            )
            .expect("STAT", "/")
            .replyRaw(std::string{statusOfARootWithDots})
            .expect("STAT", "/dir/link")
            .replyRaw(
                "213-Status of /dir/link:\r\n"
                " lrwxrwxrwx   1 ftptest  ftptest         9 Jan 02 03:04 /dir/link -> plain.txt\r\n"
                "213 End of status\r\n"
            )
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto file = lstatIn(connection, "/dir/plain.txt");
        ASSERT_TRUE(file) << file.error().toString();
        EXPECT_EQ(file->name, "plain.txt");
        EXPECT_EQ(file->size, std::optional<std::uint64_t>{11u});
        const auto link = lstatIn(connection, "/dir/link");
        ASSERT_TRUE(link) << link.error().toString();
        EXPECT_EQ(link->name, "link");
        EXPECT_EQ(link->type, SharedData::FileType::Symlink);
        EXPECT_EQ(link->linkTarget, std::optional<std::string>{"plain.txt"});
    }

    TEST_F(MetadataTests, AProbeAsksMlstWhereItWorksAndReadsAMissingPathFromItsRefusal)
    {
        dialect_->learnMachineListSingle(Support::Works);
        Script script{};
        script.reply("220 Ready")
            .expect("MLST", "/dir/a")
            .replyRaw(
                "250-Listing /dir/a\r\n"
                " modify=20240102030405;perm=adfrw;size=5;type=file; /dir/a\r\n"
                "250 End\r\n"
            )
            .expect("MLST", "/dir/link")
            .replyRaw(
                "250-Listing /dir/link\r\n"
                " type=OS.unix=slink:;size=8;modify=20240102030405; /dir/link\r\n"
                "250 End\r\n"
            )
            .expect("MLST", "/dir/missing")
            .reply("550 '/dir/missing' cannot be listed")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto file = probeIn(connection, "/dir/a");
        ASSERT_TRUE(file) << file.error().toString();
        EXPECT_EQ(file->name, "a");
        EXPECT_EQ(file->type, SharedData::FileType::Regular);
        EXPECT_EQ(file->size, std::optional<std::uint64_t>{5u});
        const auto link = probeIn(connection, "/dir/link");
        ASSERT_TRUE(link) << link.error().toString();
        EXPECT_EQ(link->name, "link");
        EXPECT_EQ(link->type, SharedData::FileType::Symlink);
        const auto missing = probeIn(connection, "/dir/missing");
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile);
    }

    TEST_F(MetadataTests, AProbeAsksSizeThenCwdWithoutMlst)
    {
        dialect_->learnSize(Support::Works);
        Script script{};
        script.reply("220 Ready")
            .expect("SIZE", "/dir/a")
            .reply("213 5")
            .expect("SIZE", "/dir/sub")
            .reply("550 Could not get file size.");
        enters(script, "/dir/sub")
            .expect("SIZE", "/dir/missing")
            .reply("550 Could not get file size.")
            .expect("CWD", "/dir/missing")
            .reply("550 Failed to change directory.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto file = probeIn(connection, "/dir/a");
        ASSERT_TRUE(file) << file.error().toString();
        EXPECT_EQ(file->type, SharedData::FileType::Regular);
        EXPECT_EQ(file->size, std::optional<std::uint64_t>{5u});
        const auto directory = probeIn(connection, "/dir/sub");
        ASSERT_TRUE(directory) << directory.error().toString();
        EXPECT_EQ(directory->name, "sub");
        EXPECT_EQ(directory->type, SharedData::FileType::Directory);
        const auto missing = probeIn(connection, "/dir/missing");
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile);
    }

    TEST_F(MetadataTests, AnMlstTheServerLacksIsLearnedAndSizeAnswers)
    {
        dialect_->learnMachineListSingle(Support::Works);
        dialect_->learnSize(Support::Works);
        Script script{};
        script.reply("220 Ready")
            .expect("MLST", "/dir/a")
            .reply("500 Unknown command.")
            .expect("SIZE", "/dir/a")
            .reply("213 5")
            .expect("SIZE", "/dir/a")
            .reply("213 5")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = probeIn(connection, "/dir/a");
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(dialect_->machineListSingle(), Support::Refused);
        const auto second = probeIn(connection, "/dir/a");
        ASSERT_TRUE(second) << second.error().toString();
        EXPECT_TRUE(dialect_->probesEntries());
    }

    TEST_F(MetadataTests, AProbeWithoutMlstAndSizeIsUnsupportedAndAsksNothing)
    {
        Script script{};
        script.reply("220 Ready").expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        EXPECT_FALSE(dialect_->probesEntries());
        const auto probed = probeIn(connection, "/dir/a");
        ASSERT_FALSE(probed);
        EXPECT_EQ(probed.error().kind, RemoteIo::RemoteErrorKind::Unsupported);
    }

    TEST_F(MetadataTests, ASizeTheServerLacksIsLearnedAndTheProbeIsUnsupported)
    {
        dialect_->learnSize(Support::Works);
        Script script{};
        script.reply("220 Ready").expect("SIZE", "/dir/a").reply("502 Command not implemented.").expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto probed = probeIn(connection, "/dir/a");
        ASSERT_FALSE(probed);
        EXPECT_EQ(probed.error().kind, RemoteIo::RemoteErrorKind::Unsupported);
        EXPECT_EQ(dialect_->size(), Support::Refused);
        EXPECT_FALSE(dialect_->probesEntries());
    }

    TEST_F(MetadataTests, ALinkWhoseTargetEndsWithItsPathKeepsTheTarget)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("STAT", "/data")
            .replyRaw(
                "213-Status follows:\r\n"
                "lrwxrwxrwx    1 0        0               9 Jan 02 03:04 data -> /mnt/data\r\n"
                "213 End of status\r\n"
            )
            .expect("STAT", "/")
            .replyRaw(std::string{statusOfARootWithDots})
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto link = lstatIn(connection, "/data");
        ASSERT_TRUE(link) << link.error().toString();
        EXPECT_EQ(link->name, "data");
        EXPECT_EQ(link->type, SharedData::FileType::Symlink);
        EXPECT_EQ(link->linkTarget, std::optional<std::string>{"/mnt/data"});
        EXPECT_EQ(dialect_->statusListsDots(), Support::Works);
    }

    TEST_F(MetadataTests, ADirectoryWithOneEntryOfItsOwnNameIsNotThatEntry)
    {
        // A server whose STAT lists a directory without "." answers the same for a file and for such a directory.
        Script script{};
        script.reply("220 Ready")
            .expect("STAT", "/backup/backup")
            .replyRaw(
                "213-Status follows:\r\n"
                "-rw-r--r--    1 0        0               5 Jan 02 03:04 backup\r\n"
                "213 End of status\r\n"
            )
            .expect("STAT", "/")
            .replyRaw(
                "213-Status follows:\r\n"
                "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 backup\r\n"
                "213 End of status\r\n"
            );
        const std::string listedDirectory = "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 backup\r\n";
        listingOf(enters(script, "/backup"), "LIST", "", listedDirectory);
        listingOf(script, "LIST", "-a", listedDirectory);
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto directory = lstatIn(connection, "/backup/backup");
        ASSERT_TRUE(directory) << directory.error().toString();
        EXPECT_EQ(directory->type, SharedData::FileType::Directory);
        EXPECT_EQ(dialect_->statusListsDots(), Support::Refused);
    }

    TEST_F(MetadataTests, AnMdtmThatReadsTheTimeAsANameGivesWayToSiteUtime)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("MFMT", "20200102030405 /a")
            .reply("502 Command not implemented.")
            .expect("MDTM", "20200102030405 /a")
            .reply("550 20200102030405 /a: No such file or directory")
            .expect("STAT", "/a")
            .replyRaw(
                "213-Status of /a:\r\n -rw-r--r--   1 ftptest  ftptest         5 Jan 02 03:04 /a\r\n213 End of "
                "status\r\n"
            )
            .expect("SITE", "UTIME 20200102030405 /a")
            .reply("200 SITE UTIME command successful")
            .expect("SITE", "UTIME 20200102030405 /b")
            .reply("200 SITE UTIME command successful")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto first = setTimeIn(connection, "/a", someTime());
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(dialect_->modificationTimeSetter(), ModificationTimeSetter::SiteUtime);
        const auto second = setTimeIn(connection, "/b", someTime());
        ASSERT_TRUE(second) << second.error().toString();
    }

    TEST_F(MetadataTests, AServerThatSetsNoTimeIsAskedOnce)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("MFMT")
            .reply("500 Unknown command.")
            .expect("MDTM")
            .reply("501 Syntax error in parameters.")
            .expect("SITE")
            .reply("500 Unknown SITE command.")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        for (auto const path : {"/a", "/b"})
        {
            const auto set = setTimeIn(connection, path, someTime());
            ASSERT_FALSE(set);
            EXPECT_EQ(set.error().kind, RemoteIo::RemoteErrorKind::Unsupported) << set.error().toString();
        }
        EXPECT_EQ(dialect_->modificationTimeSetter(), ModificationTimeSetter::None);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(MetadataTests, ARefusedMdtmOfAFileThatExistsKeepsTheSetter)
    {
        // vsftpd's answer for a file of another owner, the next file sets its time.
        Script script{};
        script.reply("220 Ready")
            .expect("MFMT", "20200102030405 /a")
            .reply("500 Unknown command.")
            .expect("MDTM", "20200102030405 /a")
            .reply("550 Could not set file modification time.")
            .expect("STAT", "/a")
            .replyRaw(
                "213-Status of /a:\r\n -rw-r--r--   1 other    other           5 Jan 02 03:04 /a\r\n213 End of "
                "status\r\n"
            )
            .expect("SITE", "UTIME 20200102030405 /a")
            .reply("500 Unknown SITE command.")
            .expect("MDTM", "20200102030405 /b")
            .reply("213 File modification time set.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto refused = setTimeIn(connection, "/a", someTime());
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().kind, RemoteIo::RemoteErrorKind::PermissionDenied) << refused.error().toString();
        EXPECT_EQ(refused.error().protocolCode, 550);
        EXPECT_EQ(dialect_->modificationTimeSetter(), ModificationTimeSetter::ModificationTimeWithTime);
        const auto set = setTimeIn(connection, "/b", someTime());
        ASSERT_TRUE(set) << set.error().toString();
    }

    TEST_F(MetadataTests, AnMdtmThatReadsTheTimeAsANameWithNoOtherFormSetsNoTime)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("MFMT")
            .reply("502 Command not implemented.")
            .expect("MDTM", "20200102030405 /a")
            .reply("550 20200102030405 /a: No such file or directory")
            .expect("STAT", "/a")
            .replyRaw(
                "213-Status of /a:\r\n -rw-r--r--   1 ftptest  ftptest         5 Jan 02 03:04 /a\r\n213 End of "
                "status\r\n"
            )
            .expect("SITE")
            .reply("500 Unknown SITE command.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto set = setTimeIn(connection, "/a", someTime());
        ASSERT_FALSE(set);
        EXPECT_EQ(set.error().kind, RemoteIo::RemoteErrorKind::Unsupported) << set.error().toString();
        EXPECT_EQ(dialect_->modificationTimeSetter(), ModificationTimeSetter::None);
    }

    TEST_F(MetadataTests, ADirectoryRefusedWhereItsParentExistsIsNotMissing)
    {
        // vsftpd's answer in a directory that takes no entries: nothing at the path, and its parent is entered.
        Script script{};
        script.reply("220 Ready")
            .expect("MKD", "/dir/new")
            .reply("550 Create directory operation failed.")
            .expect("STAT", "/dir/new")
            .replyRaw("213-Status follows:\r\n213 End of status\r\n");
        listingOf(enters(script, "/dir"), "LIST", "", std::string{listedFile});
        listingOf(script, "LIST", "-a", std::string{listedFile});
        script.expect("SIZE", "/dir").reply("550 Could not get file size.");
        enters(script, "/dir").expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto made = run(
            connection,
            [session = sessionOf(connection)]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
            {
                co_return co_await makeDirectory(session, "/dir/new");
            }
        );
        ASSERT_FALSE(made);
        EXPECT_EQ(made.error().kind, RemoteIo::RemoteErrorKind::Failure) << made.error().toString();
        EXPECT_EQ(made.error().protocolCode, 550);
    }

    TEST_F(MetadataTests, ADirectoryIsRemovedFromItsParent)
    {
        Script script{};
        script.reply("220 Ready");
        listingOf(enters(script, "/dir/sub"), "LIST", "", "");
        listingOf(script, "LIST", "-a", "");
        enters(script, "/dir").expect("RMD", "/dir/sub").reply("250 Remove directory operation successful.");
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto listed = listIn(connection, "/dir/sub");
        ASSERT_TRUE(listed) << listed.error().toString();
        const auto removed = removeDirectoryIn(connection, "/dir/sub");
        ASSERT_TRUE(removed) << removed.error().toString();
    }

    TEST_F(MetadataTests, ARefusedRmdOfADirectoryWithContentSaysSoWithoutItsStatus)
    {
        // vsftpd's refusal tells nothing. The parent tells the directory, its listing the content, no STAT lists it.
        const std::string listedDirectory = "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 sub\r\n";
        Script script{};
        script.reply("220 Ready");
        enters(script, "/dir").expect("RMD", "/dir/sub").reply("550 Remove directory operation failed.");
        listingOf(enters(script, "/dir"), "LIST", "", listedDirectory);
        listingOf(script, "LIST", "-a", listedDirectory);
        listingOf(enters(script, "/dir/sub"), "LIST", "-a", std::string{listedFile});
        script.expectDisconnect();
        const auto connection = plainConnection(std::move(script));

        const auto removed = removeDirectoryIn(connection, "/dir/sub");
        ASSERT_FALSE(removed);
        EXPECT_EQ(removed.error().kind, RemoteIo::RemoteErrorKind::Failure) << removed.error().toString();
        EXPECT_EQ(removed.error().message, "The directory is not empty");
        EXPECT_EQ(removed.error().protocolCode, 550);
    }

    TEST_F(MetadataTests, ACancelledCommandEndsCancelledAndAsksNothingMore)
    {
        using VoidResult = std::expected<void, RemoteIo::RemoteError>;
        Script script{};
        script.reply("220 Ready");
        const auto held = [&script](std::string verb, std::string argument, std::string const& name)
        {
            script.expect(std::move(verb), std::move(argument))
                .checkpoint("received-" + name)
                .awaitRelease("answer-" + name)
                .reply("550 Refused without a reason.");
        };
        held("MKD", "/a", "make");
        held("DELE", "/a", "remove");
        held("SIZE", "/a", "size");
        held("CWD", "/a", "canonical");
        script.expect("RNFR", "/a").reply("350 Ready for RNTO.");
        held("RNTO", "/b", "rename");
        script.expect("NOOP").reply("200 Fine").expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto session = sessionOf(connection);

        const auto expectCancelled = [](auto const& result, std::string_view what)
        {
            ASSERT_FALSE(result) << what << " succeeded";
            EXPECT_EQ(result.error().kind, RemoteIo::RemoteErrorKind::Cancelled)
                << what << ": " << result.error().toString();
        };
        ASSERT_NO_FATAL_FAILURE(expectCancelled(
            cancelledAt<VoidResult>(connection, makeDirectory(session, "/a"), "received-make", "answer-make"), "MKD"
        ));
        ASSERT_NO_FATAL_FAILURE(expectCancelled(
            cancelledAt<VoidResult>(connection, removeFile(session, "/a"), "received-remove", "answer-remove"), "DELE"
        ));
        ASSERT_NO_FATAL_FAILURE(expectCancelled(
            cancelledAt<std::expected<std::uint64_t, RemoteIo::RemoteError>>(
                connection, sizeOf(session, "/a"), "received-size", "answer-size"
            ),
            "SIZE"
        ));
        ASSERT_NO_FATAL_FAILURE(expectCancelled(
            cancelledAt<std::expected<std::string, RemoteIo::RemoteError>>(
                connection, canonicalDirectory(session, "/a"), "received-canonical", "answer-canonical"
            ),
            "CWD"
        ));
        ASSERT_NO_FATAL_FAILURE(expectCancelled(
            cancelledAt<VoidResult>(connection, renamePath(session, "/a", "/b"), "received-rename", "answer-rename"),
            "RNTO"
        ));
        expectReply(execute(connection, command(Verb::Noop)), 200);
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
