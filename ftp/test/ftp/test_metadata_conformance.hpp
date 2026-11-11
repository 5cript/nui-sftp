#pragma once

#ifndef _WIN32
#    include "test_metadata.hpp"
#    include "test_transfer_conformance.hpp"

#    include <ftp/client/metadata.hpp>

#    include <gtest/gtest.h>

#    include <algorithm>
#    include <memory>
#    include <set>
#    include <string>
#    include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /**
         * @brief The names servers/common/fixtures.sh puts into every workspace.
         */
        std::set<std::string> workspaceNames()
        {
            return {
                ".hidden",
                " leading space.txt",
                "a -> b.txt",
                "almost-a-chunk.bin",
                "chunk.bin",
                "dangling-link",
                "directory",
                "empty.bin",
                "large.bin",
                "link-to-directory",
                "link-to-plain.txt",
                "name with spaces.txt",
                "one.bin",
                "plain.txt",
                "unreadable.txt",
                "unwritable",
                std::string(251u, 'n') + ".txt",
                "\xc3\xbcn\xc3\xaf"
                "c\xc3\xb6"
                "d\xc3\xa9-\xe5\x90\x8d\xe5\x89\x8d.txt",
            };
        }

        std::set<std::string> nameSetOf(Listing const& listing)
        {
            std::set<std::string> names{};
            for (auto const& entry : listing.entries)
                names.insert(entry.name);
            return names;
        }

        FileTransferProtocol::ListingEntry const* findEntry(Listing const& listing, std::string_view name)
        {
            const auto found = std::ranges::find(listing.entries, name, &FileTransferProtocol::ListingEntry::name);
            return found == listing.entries.end() ? nullptr : &*found;
        }
    }

    /**
     * @brief The metadata operations against every selected server, plain and over both kinds of TLS, each case in a
     *        copy of the fixtures of its own.
     */
    class MetadataConformance : public TransferConformance
    {
      protected:
        MetadataSession metadataSession() const
        {
            return MetadataSession{.transfer = session_, .dialect = dialect_, .options = transferOptions()};
        }

        ListingResult listIn(std::string path)
        {
            return onStrand(
                connection_,
                [session = metadataSession(), path = std::move(path)]() -> boost::asio::awaitable<ListingResult>
                {
                    co_return co_await listDirectory(session, path);
                }
            );
        }

        using EntryResult = std::expected<FileTransferProtocol::ListingEntry, RemoteIo::RemoteError>;

        EntryResult lstatOf(std::string path)
        {
            return onStrand(
                connection_,
                [session = metadataSession(), path = std::move(path)]() -> boost::asio::awaitable<EntryResult>
                {
                    co_return co_await lstatPath(session, path);
                }
            );
        }

        EntryResult statOf(std::string path)
        {
            return onStrand(
                connection_,
                [session = metadataSession(), path = std::move(path)]() -> boost::asio::awaitable<EntryResult>
                {
                    co_return co_await statPath(session, path);
                }
            );
        }

        static void expectEntry(
            EntryResult const& result,
            std::string_view name,
            SharedData::FileType type,
            std::optional<std::uint64_t> size = std::nullopt
        )
        {
            ASSERT_TRUE(result) << name << ": " << result.error().toString();
            EXPECT_EQ(result->name, name);
            EXPECT_EQ(result->type, type) << name;
            if (size)
                EXPECT_EQ(result->size, size) << name;
        }

        static void expectMissing(EntryResult const& result, std::string_view path)
        {
            ASSERT_FALSE(result) << path << " was found";
            EXPECT_EQ(result.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile)
                << path << ": " << result.error().toString();
        }

        const std::shared_ptr<ServerDialect> dialect_ = std::make_shared<ServerDialect>();
    };

    TEST_P(MetadataConformance, ListsEveryFixtureWithItsType)
    {
        const auto listed = listIn(workspace_);
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(nameSetOf(*listed), workspaceNames());
        EXPECT_EQ(listed->unreadableLines, 0u);
        EXPECT_EQ(listed->refusedNames, 0u);

        for (
            auto const& [name, size] :
            {std::pair<std::string_view, std::uint64_t>{"empty.bin", 0u},
                {"one.bin", 1u},
                {"chunk.bin", 16384u},
                {"large.bin", 10485760u},
                {"plain.txt", 11u}}
        )
        {
            auto const* const entry = findEntry(*listed, name);
            ASSERT_NE(entry, nullptr) << name;
            EXPECT_EQ(entry->type, SharedData::FileType::Regular) << name;
            EXPECT_EQ(entry->size, std::optional<std::uint64_t>{size}) << name;
            EXPECT_TRUE(entry->modified || entry->listedTime) << name << " has no time";
        }
        for (auto const name : {"directory", "unwritable"})
        {
            auto const* const entry = findEntry(*listed, name);
            ASSERT_NE(entry, nullptr) << name;
            EXPECT_EQ(entry->type, SharedData::FileType::Directory) << name;
        }
        for (
            auto const& [name, target] :
            {std::pair<std::string_view, std::string_view>{"link-to-plain.txt", "plain.txt"},
                {"link-to-directory", "directory"},
                {"dangling-link", "does-not-exist"}}
        )
        {
            auto const* const entry = findEntry(*listed, name);
            ASSERT_NE(entry, nullptr) << name;
            EXPECT_EQ(entry->type, SharedData::FileType::Symlink) << name;
            EXPECT_EQ(entry->linkTarget, std::optional<std::string>{std::string{target}}) << name;
        }
        auto const* const arrowName = findEntry(*listed, "a -> b.txt");
        ASSERT_NE(arrowName, nullptr);
        EXPECT_EQ(arrowName->type, SharedData::FileType::Regular);
    }

    TEST_P(MetadataConformance, LearnsTheListingCommandOnce)
    {
        const auto first = listIn(workspace_);
        ASSERT_TRUE(first) << first.error().toString();
        const auto nested = listIn(workspace_ + "/directory");
        ASSERT_TRUE(nested) << nested.error().toString();
        EXPECT_EQ(nameSetOf(*nested), std::set<std::string>{"nested"});

        EXPECT_EQ(dialect_->listAll(), Support::Works);
        EXPECT_EQ(dialect_->machineListing(), Support::Unknown) << "LIST had lines the parser could not read";
    }

    TEST_P(MetadataConformance, AMissingDirectoryIsNotListedAndTheLoginStays)
    {
        const auto missing = listIn(workspace_ + "/does-not-exist");
        ASSERT_FALSE(missing) << "listed " << missing->entries.size() << " entries";
        EXPECT_EQ(missing.error().protocolCode, 550) << missing.error().toString();
        if (profile_.expectations.repliesTell550Causes)
            EXPECT_EQ(missing.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile) << missing.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
    }

    TEST_P(MetadataConformance, AFileIsNotListedAsADirectory)
    {
        const auto file = listIn(workspace_ + "/plain.txt");
        ASSERT_FALSE(file) << "listed " << file->entries.size() << " entries";
        EXPECT_EQ(file.error().protocolCode, 550) << file.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
    }

    TEST_P(MetadataConformance, LstatTellsEveryKindOfEntry)
    {
        using SharedData::FileType;
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("plain.txt")), "plain.txt", FileType::Regular, 11u));
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(lstatOf(pathOf(" leading space.txt")), " leading space.txt", FileType::Regular, 8u)
        );
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("a -> b.txt")), "a -> b.txt", FileType::Regular, 6u));
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(lstatOf(pathOf("unreadable.txt")), "unreadable.txt", FileType::Regular, 7u)
        );
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("directory")), "directory", FileType::Directory));
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("unwritable")), "unwritable", FileType::Directory));
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("unwritable/file.txt")), "file.txt", FileType::Regular, 7u));
        for (
            auto const& [name, target] :
            {std::pair<std::string_view, std::string_view>{"link-to-plain.txt", "plain.txt"},
                {"link-to-directory", "directory"},
                {"dangling-link", "does-not-exist"}}
        )
        {
            const auto link = lstatOf(pathOf(name));
            ASSERT_NO_FATAL_FAILURE(expectEntry(link, name, FileType::Symlink));
            EXPECT_EQ(link->linkTarget, std::optional<std::string>{std::string{target}}) << name;
        }
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf("/"), "/", FileType::Directory));
    }

    TEST_P(MetadataConformance, StatFollowsLinks)
    {
        using SharedData::FileType;
        const auto plain = statOf(pathOf("plain.txt"));
        ASSERT_NO_FATAL_FAILURE(expectEntry(plain, "plain.txt", FileType::Regular, 11u));
        EXPECT_TRUE(plain->modified) << "MLST and MDTM give the exact time";
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(statOf(pathOf("link-to-plain.txt")), "link-to-plain.txt", FileType::Regular, 11u)
        );
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(statOf(pathOf("link-to-directory")), "link-to-directory", FileType::Directory)
        );
        ASSERT_NO_FATAL_FAILURE(expectEntry(statOf(pathOf("directory")), "directory", FileType::Directory));
        ASSERT_NO_FATAL_FAILURE(expectEntry(statOf(pathOf("unreadable.txt")), "unreadable.txt", FileType::Regular, 7u));
        ASSERT_NO_FATAL_FAILURE(expectMissing(statOf(pathOf("dangling-link")), "dangling-link"));
    }

    TEST_P(MetadataConformance, MissingPathsAreNoSuchFileOnEveryServer)
    {
        for (
            auto const name :
            {"does-not-exist", "does-not-exist/child", "does-not-exist/deeper/child", "plain.txt/child"}
        )
        {
            ASSERT_NO_FATAL_FAILURE(expectMissing(lstatOf(pathOf(name)), name));
            ASSERT_NO_FATAL_FAILURE(expectMissing(statOf(pathOf(name)), name));
        }
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        MetadataConformance,
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
