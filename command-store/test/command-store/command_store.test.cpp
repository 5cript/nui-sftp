#include <command-store/command_store.hpp>
#include <utility/temporary_directory.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

std::filesystem::path programDirectory;

namespace
{
    using CommandStore::HistoryEntry;
    using CommandStore::HistoryQuery;
    using CommandStore::ImportEntry;
    using CommandStore::ImportSummary;
    using CommandStore::Result;
    using CommandStore::Snippet;
    using CommandStore::SnippetFolder;
    using CommandStore::SortOrder;
    using CommandStore::Store;

    class CommandStoreTests : public ::testing::Test
    {
      protected:
        void openStore(std::size_t historyCap = Store::defaultHistoryCap)
        {
            auto opened = Store::open(context_.get_executor(), databaseFile(), historyCap);
            ASSERT_TRUE(opened.has_value()) << opened.error().message;
            store_.emplace(std::move(*opened));
        }

        void closeStore()
        {
            store_.reset();
        }

        std::filesystem::path databaseFile() const
        {
            return temporaryDirectory_.path() / "command_store.db";
        }

        /**
         * @brief Invokes an asynchronous store method and pumps the io_context until its callback fired.
         */
        template <typename ValueT, typename InvokeT>
        Result<ValueT> await(InvokeT&& invoke)
        {
            std::optional<Result<ValueT>> captured{};
            std::forward<InvokeT>(invoke)(
                [&captured](Result<ValueT> result)
                {
                    captured = std::move(result);
                }
            );
            context_.restart();
            context_.run();
            if (!captured)
                throw std::runtime_error{"store callback was not invoked"};
            return std::move(*captured);
        }

        Result<HistoryEntry> record(std::string host, std::string command, std::int64_t nowEpoch)
        {
            return await<HistoryEntry>(
                [&](auto&& onComplete)
                {
                    store_->recordExecution(
                        std::move(host), std::move(command), nowEpoch, std::forward<decltype(onComplete)>(onComplete)
                    );
                }
            );
        }

        Result<std::vector<HistoryEntry>> listHistory(HistoryQuery query = {})
        {
            return await<std::vector<HistoryEntry>>(
                [&](auto&& onComplete)
                {
                    store_->listHistory(std::move(query), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> setHistoryFlags(std::int64_t id, std::optional<bool> pinned, std::optional<bool> favorite)
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->setHistoryFlags(id, pinned, favorite, std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> deleteHistory(std::vector<std::int64_t> ids)
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->deleteHistory(std::move(ids), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> clearHistory()
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->clearHistory(std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<std::vector<Snippet>> listSnippets()
        {
            return await<std::vector<Snippet>>(
                [&](auto&& onComplete)
                {
                    store_->listSnippets(std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<Snippet> upsertSnippet(Snippet snippet)
        {
            return await<Snippet>(
                [&](auto&& onComplete)
                {
                    store_->upsertSnippet(std::move(snippet), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> deleteSnippet(std::string id)
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->deleteSnippet(std::move(id), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> deleteSnippets(std::vector<std::string> ids)
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->deleteSnippets(std::move(ids), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> bumpSnippetUse(std::string id, std::int64_t nowEpoch)
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->bumpSnippetUse(std::move(id), nowEpoch, std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<std::vector<SnippetFolder>> listFolders()
        {
            return await<std::vector<SnippetFolder>>(
                [&](auto&& onComplete)
                {
                    store_->listFolders(std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<SnippetFolder> upsertFolder(SnippetFolder folder)
        {
            return await<SnippetFolder>(
                [&](auto&& onComplete)
                {
                    store_->upsertFolder(std::move(folder), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<void> deleteFolder(std::string id)
        {
            return await<void>(
                [&](auto&& onComplete)
                {
                    store_->deleteFolder(std::move(id), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        Result<ImportSummary> importSnippets(std::vector<ImportEntry> entries)
        {
            return await<ImportSummary>(
                [&](auto&& onComplete)
                {
                    store_->importSnippets(std::move(entries), std::forward<decltype(onComplete)>(onComplete));
                }
            );
        }

        std::vector<std::string> commandsOf(std::vector<HistoryEntry> const& entries) const
        {
            std::vector<std::string> commands{};
            commands.reserve(entries.size());
            for (auto const& entry : entries)
                commands.push_back(entry.command);
            return commands;
        }

      protected:
        boost::asio::io_context context_{};
        Utility::TemporaryDirectory temporaryDirectory_{programDirectory / "temp", true};
        std::optional<Store> store_{};
    };

    TEST_F(CommandStoreTests, RecordingACommandCreatesAnEntry)
    {
        openStore();
        const auto entry = record("user@host", "ls -la", 100);
        ASSERT_TRUE(entry.has_value()) << entry.error().message;
        EXPECT_GT(entry->id, 0);
        EXPECT_EQ(entry->host, "user@host");
        EXPECT_EQ(entry->command, "ls -la");
        EXPECT_EQ(entry->firstRun, 100);
        EXPECT_EQ(entry->lastRun, 100);
        EXPECT_EQ(entry->runs, 1);
        EXPECT_FALSE(entry->pinned);
        EXPECT_FALSE(entry->favorite);

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 1u);
    }

    TEST_F(CommandStoreTests, RepeatedExecutionIncrementsRunsAndKeepsFirstRun)
    {
        openStore();
        ASSERT_TRUE(record("user@host", "ls -la", 100).has_value());
        const auto second = record("user@host", "ls -la", 200);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(second->runs, 2);
        EXPECT_EQ(second->firstRun, 100);
        EXPECT_EQ(second->lastRun, 200);

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 1u);
    }

    TEST_F(CommandStoreTests, SameCommandOnDifferentHostsStaysSeparate)
    {
        openStore();
        ASSERT_TRUE(record("alpha", "ls", 100).has_value());
        ASSERT_TRUE(record("beta", "ls", 200).has_value());

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(entries->size(), 2u);
    }

    TEST_F(CommandStoreTests, HistoryCapTrimsOldestNonPinnedEntries)
    {
        openStore(3);
        for (std::int64_t index = 0; index != 5; ++index)
            ASSERT_TRUE(record("host", "command " + std::to_string(index), 100 + index).has_value());

        const auto entries = listHistory({.sort = SortOrder::Recent});
        ASSERT_TRUE(entries.has_value());
        const auto commands = commandsOf(*entries);
        EXPECT_EQ(commands, (std::vector<std::string>{"command 4", "command 3", "command 2"}));
    }

    TEST_F(CommandStoreTests, PinnedEntriesSurviveCapTrimming)
    {
        openStore(2);
        const auto pinnedEntry = record("host", "keep me", 100);
        ASSERT_TRUE(pinnedEntry.has_value());
        ASSERT_TRUE(setHistoryFlags(pinnedEntry->id, true, std::nullopt).has_value());

        for (std::int64_t index = 0; index != 3; ++index)
            ASSERT_TRUE(record("host", "filler " + std::to_string(index), 200 + index).has_value());

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        const auto commands = commandsOf(*entries);
        EXPECT_TRUE(std::ranges::contains(commands, "keep me"));
    }

    TEST_F(CommandStoreTests, ListHistorySortsByRecent)
    {
        openStore();
        ASSERT_TRUE(record("host", "oldest", 100).has_value());
        ASSERT_TRUE(record("host", "newest", 300).has_value());
        ASSERT_TRUE(record("host", "middle", 200).has_value());

        const auto entries = listHistory({.sort = SortOrder::Recent});
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(commandsOf(*entries), (std::vector<std::string>{"newest", "middle", "oldest"}));
    }

    TEST_F(CommandStoreTests, ListHistorySortsByMostRun)
    {
        openStore();
        ASSERT_TRUE(record("host", "once", 100).has_value());
        for (std::int64_t index = 0; index != 3; ++index)
            ASSERT_TRUE(record("host", "thrice", 200 + index).has_value());
        for (std::int64_t index = 0; index != 2; ++index)
            ASSERT_TRUE(record("host", "twice", 300 + index).has_value());

        const auto entries = listHistory({.sort = SortOrder::MostRun});
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(commandsOf(*entries), (std::vector<std::string>{"thrice", "twice", "once"}));
    }

    TEST_F(CommandStoreTests, ListHistorySortsByNameCaseInsensitively)
    {
        openStore();
        ASSERT_TRUE(record("host", "beta", 100).has_value());
        ASSERT_TRUE(record("host", "Alpha", 200).has_value());
        ASSERT_TRUE(record("host", "charlie", 300).has_value());

        const auto entries = listHistory({.sort = SortOrder::Name});
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(commandsOf(*entries), (std::vector<std::string>{"Alpha", "beta", "charlie"}));
    }

    TEST_F(CommandStoreTests, ListHistoryFiltersByHost)
    {
        openStore();
        ASSERT_TRUE(record("alpha", "on alpha", 100).has_value());
        ASSERT_TRUE(record("beta", "on beta", 200).has_value());

        const auto entries = listHistory({.hostFilter = "alpha"});
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 1u);
        EXPECT_EQ(entries->front().command, "on alpha");
    }

    TEST_F(CommandStoreTests, ListHistoryRespectsLimit)
    {
        openStore();
        for (std::int64_t index = 0; index != 5; ++index)
            ASSERT_TRUE(record("host", "command " + std::to_string(index), 100 + index).has_value());

        const auto entries = listHistory({.sort = SortOrder::Recent, .limit = 2});
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(commandsOf(*entries), (std::vector<std::string>{"command 4", "command 3"}));
    }

    TEST_F(CommandStoreTests, HistoryFlagsToggleIndependently)
    {
        openStore();
        const auto entry = record("host", "ls", 100);
        ASSERT_TRUE(entry.has_value());

        ASSERT_TRUE(setHistoryFlags(entry->id, true, std::nullopt).has_value());
        {
            const auto entries = listHistory();
            ASSERT_TRUE(entries.has_value());
            EXPECT_TRUE(entries->front().pinned);
            EXPECT_FALSE(entries->front().favorite);
        }

        ASSERT_TRUE(setHistoryFlags(entry->id, std::nullopt, true).has_value());
        {
            const auto entries = listHistory();
            ASSERT_TRUE(entries.has_value());
            EXPECT_TRUE(entries->front().pinned);
            EXPECT_TRUE(entries->front().favorite);
        }
    }

    TEST_F(CommandStoreTests, SettingFlagsOnUnknownEntryFails)
    {
        openStore();
        const auto result = setHistoryFlags(4711, true, std::nullopt);
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(CommandStoreTests, DeleteHistoryRemovesOnlyGivenIds)
    {
        openStore();
        const auto first = record("host", "first", 100);
        const auto second = record("host", "second", 200);
        const auto third = record("host", "third", 300);
        ASSERT_TRUE(first.has_value() && second.has_value() && third.has_value());

        ASSERT_TRUE(deleteHistory({first->id, third->id, 999'999}).has_value());

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(commandsOf(*entries), (std::vector<std::string>{"second"}));
    }

    TEST_F(CommandStoreTests, ClearHistoryRemovesEverythingIncludingPinned)
    {
        openStore();
        const auto pinnedEntry = record("host", "pinned", 100);
        ASSERT_TRUE(pinnedEntry.has_value());
        ASSERT_TRUE(setHistoryFlags(pinnedEntry->id, true, std::nullopt).has_value());
        ASSERT_TRUE(record("host", "plain", 200).has_value());

        ASSERT_TRUE(clearHistory().has_value());

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        EXPECT_TRUE(entries->empty());
    }

    TEST_F(CommandStoreTests, BinaryUnsafeLookingCommandsRoundTripIntact)
    {
        openStore();
        std::string command{"echo \"a\nb\" '$(pwd)' {{variable}} \xF0\x9F\x9A\x80 "};
        command.push_back('\0');
        command += "after nul";

        ASSERT_TRUE(record("host", command, 100).has_value());
        const auto again = record("host", command, 200);
        ASSERT_TRUE(again.has_value());
        EXPECT_EQ(again->runs, 2);

        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 1u);
        EXPECT_EQ(entries->front().command, command);
    }

    TEST_F(CommandStoreTests, UpsertSnippetGeneratesIdAndRoundTripsAllFields)
    {
        openStore();
        const auto stored = upsertSnippet({
            .name = "List",
            .command = "ls {{directory}}",
            .folder = "folder-1",
            .tags = {"files", "quick"},
            .favorite = true,
            .danger = CommandStore::DangerLevel::Caution,
        });
        ASSERT_TRUE(stored.has_value()) << stored.error().message;
        EXPECT_FALSE(stored->id.empty());
        EXPECT_EQ(stored->danger, CommandStore::DangerLevel::Caution);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        const auto& snippet = snippets->front();
        EXPECT_EQ(snippet.id, stored->id);
        EXPECT_EQ(snippet.name, "List");
        EXPECT_EQ(snippet.command, "ls {{directory}}");
        EXPECT_EQ(snippet.folder, "folder-1");
        EXPECT_EQ(snippet.tags, (std::vector<std::string>{"files", "quick"}));
        EXPECT_TRUE(snippet.favorite);
        EXPECT_EQ(snippet.danger, CommandStore::DangerLevel::Caution);
        EXPECT_EQ(snippet.uses, 0);
        EXPECT_EQ(snippet.lastUsed, 0);
    }

    TEST_F(CommandStoreTests, UpdatingASnippetPreservesUsageCounters)
    {
        openStore();
        const auto stored = upsertSnippet({.name = "List", .command = "ls"});
        ASSERT_TRUE(stored.has_value());
        ASSERT_TRUE(bumpSnippetUse(stored->id, 500).has_value());

        auto changed = *stored;
        changed.name = "List (renamed)";
        const auto updated = upsertSnippet(changed);
        ASSERT_TRUE(updated.has_value());
        EXPECT_EQ(updated->name, "List (renamed)");
        EXPECT_EQ(updated->uses, 1);
        EXPECT_EQ(updated->lastUsed, 500);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
    }

    TEST_F(CommandStoreTests, BumpSnippetUseFailsForUnknownId)
    {
        openStore();
        EXPECT_FALSE(bumpSnippetUse("does-not-exist", 100).has_value());
    }

    TEST_F(CommandStoreTests, DeleteSnippetIsIdempotent)
    {
        openStore();
        const auto stored = upsertSnippet({.name = "List", .command = "ls"});
        ASSERT_TRUE(stored.has_value());

        EXPECT_TRUE(deleteSnippet(stored->id).has_value());
        EXPECT_TRUE(deleteSnippet(stored->id).has_value());

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        EXPECT_TRUE(snippets->empty());
    }

    TEST_F(CommandStoreTests, DeleteSnippetsRemovesOnlyTheGivenIds)
    {
        openStore();
        const auto first = upsertSnippet({.name = "List", .command = "ls"});
        const auto second = upsertSnippet({.name = "Top", .command = "top"});
        const auto kept = upsertSnippet({.name = "Free", .command = "free -h"});
        ASSERT_TRUE(first.has_value() && second.has_value() && kept.has_value());

        EXPECT_TRUE(deleteSnippets({first->id, second->id, "unknown"}).has_value());

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().id, kept->id);
    }

    TEST_F(CommandStoreTests, FoldersRoundTripAndOrderByPosition)
    {
        openStore();
        const auto second = upsertFolder({.name = "Second", .icon = "folder", .position = 2});
        const auto first = upsertFolder({.name = "First", .icon = "open-folder", .position = 1});
        ASSERT_TRUE(second.has_value() && first.has_value());
        EXPECT_FALSE(first->id.empty());

        const auto folders = listFolders();
        ASSERT_TRUE(folders.has_value());
        ASSERT_EQ(folders->size(), 2u);
        EXPECT_EQ(folders->front().name, "First");
        EXPECT_EQ(folders->front().icon, "open-folder");
        EXPECT_EQ(folders->back().name, "Second");
    }

    TEST_F(CommandStoreTests, DeletingAFolderMovesItsSnippetsToRoot)
    {
        openStore();
        const auto folder = upsertFolder({.name = "Scripts"});
        ASSERT_TRUE(folder.has_value());
        const auto snippet = upsertSnippet({.name = "List", .command = "ls", .folder = folder->id});
        ASSERT_TRUE(snippet.has_value());

        ASSERT_TRUE(deleteFolder(folder->id).has_value());

        const auto folders = listFolders();
        ASSERT_TRUE(folders.has_value());
        EXPECT_TRUE(folders->empty());

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().folder, "");
    }

    TEST_F(CommandStoreTests, ImportCreatesMissingFoldersByName)
    {
        openStore();
        ASSERT_TRUE(upsertFolder({.name = "Existing", .position = 4}).has_value());

        const auto summary = importSnippets({
            {.snippet = {.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}, .favorite = true}},
            {.snippet = {.name = "Copy", .command = "cp {{a}} {{b}}", .folder = "Files"}},
        });
        ASSERT_TRUE(summary.has_value()) << summary.error().message;
        EXPECT_EQ(summary->added, 2);
        EXPECT_EQ(summary->replaced, 0);
        EXPECT_EQ(summary->foldersCreated, 1);

        const auto folders = listFolders();
        ASSERT_TRUE(folders.has_value());
        ASSERT_EQ(folders->size(), 2u);
        EXPECT_EQ(folders->back().name, "Files");
        EXPECT_EQ(folders->back().position, 5);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 2u);
        for (auto const& snippet : *snippets)
            EXPECT_EQ(snippet.folder, folders->back().id);
        EXPECT_EQ(snippets->back().name, "List");
        EXPECT_EQ(snippets->back().tags, (std::vector<std::string>{"quick"}));
        EXPECT_TRUE(snippets->back().favorite);
    }

    TEST_F(CommandStoreTests, ImportReusesExistingFolderWithSameName)
    {
        openStore();
        const auto folder = upsertFolder({.name = "Files"});
        ASSERT_TRUE(folder.has_value());

        const auto summary = importSnippets({{.snippet = {.name = "List", .command = "ls", .folder = "Files"}}});
        ASSERT_TRUE(summary.has_value()) << summary.error().message;
        EXPECT_EQ(summary->foldersCreated, 0);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().folder, folder->id);
    }

    TEST_F(CommandStoreTests, ImportPrefersTheFolderIdOverTheFolderName)
    {
        openStore();
        const auto folder = upsertFolder({.name = "Target"});
        ASSERT_TRUE(folder.has_value());

        const auto summary = importSnippets({
            {.snippet = {.name = "List", .command = "ls", .folder = "Target"}, .folderId = folder->id},
        });
        ASSERT_TRUE(summary.has_value()) << summary.error().message;
        EXPECT_EQ(summary->foldersCreated, 0);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().folder, folder->id);
    }

    TEST_F(CommandStoreTests, ImportIntoAMissingFolderIdFailsWithoutWriting)
    {
        openStore();
        const auto summary = importSnippets({
            {.snippet = {.name = "Fine", .command = "true"}},
            {.snippet = {.name = "List", .command = "ls"}, .folderId = "deleted-folder"},
        });
        EXPECT_FALSE(summary.has_value());

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        EXPECT_TRUE(snippets->empty());
    }

    TEST_F(CommandStoreTests, ImportReplacesASnippetKeepingItsUsage)
    {
        openStore();
        const auto stored = upsertSnippet({.name = "List", .command = "ls"});
        ASSERT_TRUE(stored.has_value());
        ASSERT_TRUE(bumpSnippetUse(stored->id, 500).has_value());

        const auto summary = importSnippets({
            {.snippet = {.name = "List", .command = "ls -la", .tags = {"new"}}, .replaces = stored->id},
        });
        ASSERT_TRUE(summary.has_value()) << summary.error().message;
        EXPECT_EQ(summary->added, 0);
        EXPECT_EQ(summary->replaced, 1);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().id, stored->id);
        EXPECT_EQ(snippets->front().command, "ls -la");
        EXPECT_EQ(snippets->front().tags, (std::vector<std::string>{"new"}));
        EXPECT_EQ(snippets->front().uses, 1);
    }

    TEST_F(CommandStoreTests, ImportAddsEverythingItIsGiven)
    {
        openStore();
        ASSERT_TRUE(upsertSnippet({.name = "List", .command = "ls"}).has_value());

        const auto summary = importSnippets({{.snippet = {.name = "List", .command = "ls"}}});
        ASSERT_TRUE(summary.has_value()) << summary.error().message;
        EXPECT_EQ(summary->added, 1);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        EXPECT_EQ(snippets->size(), 2u);
    }

    TEST_F(CommandStoreTests, ImportWithoutFolderGoesToRoot)
    {
        openStore();
        const auto summary = importSnippets({{.snippet = {.name = "List", .command = "ls"}}});
        ASSERT_TRUE(summary.has_value()) << summary.error().message;
        EXPECT_EQ(summary->foldersCreated, 0);

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().folder, "");
        EXPECT_FALSE(snippets->front().id.empty());
    }

    TEST_F(CommandStoreTests, DataSurvivesReopening)
    {
        openStore();
        ASSERT_TRUE(record("host", "ls -la", 100).has_value());
        const auto snippet = upsertSnippet({.name = "List", .command = "ls", .tags = {"files"}});
        ASSERT_TRUE(snippet.has_value());
        closeStore();

        openStore();
        const auto entries = listHistory();
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 1u);
        EXPECT_EQ(entries->front().command, "ls -la");

        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().tags, (std::vector<std::string>{"files"}));
    }

    TEST_F(CommandStoreTests, UnratedSnippetStaysUnrated)
    {
        openStore();
        ASSERT_TRUE(upsertSnippet({.name = "List", .command = "ls"}).has_value());
        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value());
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().danger, std::nullopt);
    }

    TEST_F(CommandStoreTests, VersionOneDatabaseIsMigratedKeepingItsSnippets)
    {
        {
            sqlite3* rawDatabase = nullptr;
            ASSERT_EQ(sqlite3_open(databaseFile().string().c_str(), &rawDatabase), SQLITE_OK);
            const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
            ASSERT_EQ(
                sqlite3_exec(
                    database.get(),
                    "CREATE TABLE history (id INTEGER PRIMARY KEY, host TEXT NOT NULL, command TEXT NOT NULL, "
                    "first_run INTEGER NOT NULL, last_run INTEGER NOT NULL, runs INTEGER NOT NULL DEFAULT 1, "
                    "pinned INTEGER NOT NULL DEFAULT 0, favorite INTEGER NOT NULL DEFAULT 0, UNIQUE(host, command));"
                    "CREATE TABLE snippet_folders (id TEXT PRIMARY KEY, name TEXT NOT NULL, "
                    "icon TEXT NOT NULL DEFAULT '', position INTEGER NOT NULL DEFAULT 0);"
                    "CREATE TABLE snippets (id TEXT PRIMARY KEY, name TEXT NOT NULL, command TEXT NOT NULL, "
                    "folder TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]', "
                    "favorite INTEGER NOT NULL DEFAULT 0, uses INTEGER NOT NULL DEFAULT 0, "
                    "last_used INTEGER NOT NULL DEFAULT 0);"
                    "INSERT INTO snippets(id, name, command) VALUES('old', 'Old', 'ls');"
                    "PRAGMA user_version = 1;",
                    nullptr,
                    nullptr,
                    nullptr
                ),
                SQLITE_OK
            );
        }

        openStore();
        const auto snippets = listSnippets();
        ASSERT_TRUE(snippets.has_value()) << snippets.error().message;
        ASSERT_EQ(snippets->size(), 1u);
        EXPECT_EQ(snippets->front().id, "old");
        EXPECT_EQ(snippets->front().danger, std::nullopt);

        auto changed = snippets->front();
        changed.danger = CommandStore::DangerLevel::Danger;
        ASSERT_TRUE(upsertSnippet(changed).has_value());
        closeStore();

        openStore();
        const auto reopened = listSnippets();
        ASSERT_TRUE(reopened.has_value());
        ASSERT_EQ(reopened->size(), 1u);
        EXPECT_EQ(reopened->front().danger, CommandStore::DangerLevel::Danger);
    }

    TEST_F(CommandStoreTests, ReadOnlyDatabaseFileFailsToOpen)
    {
        if (geteuid() == 0)
            GTEST_SKIP() << "root ignores file permissions";
        openStore();
        closeStore();

        namespace fs = std::filesystem;
        fs::permissions(databaseFile(), fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
        auto opened = Store::open(context_.get_executor(), databaseFile(), Store::defaultHistoryCap);
        fs::permissions(databaseFile(), fs::perms::owner_read | fs::perms::owner_write);

        ASSERT_FALSE(opened.has_value());
        EXPECT_NE(opened.error().message.find("-shm' are writable"), std::string::npos) << opened.error().message;
    }

    TEST_F(CommandStoreTests, ReadOnlySharedMemoryFileFailsToOpen)
    {
        if (geteuid() == 0)
            GTEST_SKIP() << "root ignores file permissions";
        openStore();
        closeStore();

        // What chmod 444 on the database leaves behind once the app created the -shm file: the
        // database is writable again, the -shm file is not.
        namespace fs = std::filesystem;
        const auto sharedMemoryFile = fs::path{databaseFile().string() + "-shm"};
        std::ofstream{sharedMemoryFile};
        fs::permissions(sharedMemoryFile, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
        auto opened = Store::open(context_.get_executor(), databaseFile(), Store::defaultHistoryCap);
        fs::permissions(sharedMemoryFile, fs::perms::owner_read | fs::perms::owner_write);

#ifdef __APPLE__
        // Apple's SQLite replaces a stale read-only -shm file, the store just opens.
        EXPECT_TRUE(opened.has_value()) << opened.error().message;
        return;
#endif
        ASSERT_FALSE(opened.has_value());
        EXPECT_NE(opened.error().message.find("-shm' are writable"), std::string::npos) << opened.error().message;
    }
}

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
