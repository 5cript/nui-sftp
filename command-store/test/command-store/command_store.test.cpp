#include <command-store/command_store.hpp>
#include <utility/temporary_directory.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
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
        });
        ASSERT_TRUE(stored.has_value()) << stored.error().message;
        EXPECT_FALSE(stored->id.empty());

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
}

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
