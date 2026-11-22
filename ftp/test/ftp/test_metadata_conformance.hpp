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

        EntryResult probeOf(std::string path)
        {
            return onStrand(
                connection_,
                [session = metadataSession(), path = std::move(path)]() -> boost::asio::awaitable<EntryResult>
                {
                    co_return co_await probePath(session, path);
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

        using VoidResult = std::expected<void, RemoteIo::RemoteError>;

        /**
         * @brief Runs what @p operation returns for the metadata session, on the strand of the connection.
         */
        template <typename ResultT, typename OperationT>
        ResultT perform(OperationT operation)
        {
            return onStrand(
                connection_,
                [session = metadataSession(), operation = std::move(operation)]() -> boost::asio::awaitable<ResultT>
                {
                    co_return co_await operation(session);
                }
            );
        }

        VoidResult makeDirectoryAt(std::string path)
        {
            return perform<VoidResult>(
                [path](MetadataSession session)
                {
                    return makeDirectory(std::move(session), path);
                }
            );
        }

        VoidResult removeFileAt(std::string path)
        {
            return perform<VoidResult>(
                [path](MetadataSession session)
                {
                    return removeFile(std::move(session), path);
                }
            );
        }

        VoidResult removeDirectoryAt(std::string path)
        {
            return perform<VoidResult>(
                [path](MetadataSession session)
                {
                    return removeDirectory(std::move(session), path);
                }
            );
        }

        VoidResult rename(std::string from, std::string to)
        {
            return perform<VoidResult>(
                [from, to](MetadataSession session)
                {
                    return renamePath(std::move(session), from, to);
                }
            );
        }

        template <typename ValueT>
        static void expectValue(
            std::expected<ValueT, RemoteIo::RemoteError> const& result,
            ValueT const& value,
            std::string_view what
        )
        {
            ASSERT_TRUE(result) << what << ": " << result.error().toString();
            EXPECT_EQ(*result, value) << what;
        }

        /**
         * @brief @p result is a refusal in a directory that exists and takes no entries: never NoSuchFile, and
         *        PermissionDenied where the server tells.
         */
        void expectRefusedInItsDirectory(VoidResult const& result, std::string_view what) const
        {
            using RemoteIo::RemoteErrorKind;
            ASSERT_FALSE(result) << what << " succeeded";
            EXPECT_NE(result.error().kind, RemoteErrorKind::NoSuchFile) << what << ": " << result.error().toString();
            EXPECT_NE(result.error().kind, RemoteErrorKind::AlreadyExists)
                << what << ": " << result.error().toString();
            if (profile_.expectations.repliesTell550Causes)
                EXPECT_EQ(result.error().kind, RemoteErrorKind::PermissionDenied)
                    << what << ": " << result.error().toString();
        }

        static void expectFailure(VoidResult const& result, RemoteIo::RemoteErrorKind kind, std::string_view what)
        {
            ASSERT_FALSE(result) << what << " succeeded";
            EXPECT_EQ(result.error().kind, kind) << what << ": " << result.error().toString();
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
        EXPECT_NE(dialect_->statusListsDots(), Support::Refused) << "STAT answers no lstat of a file";
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

    TEST_P(MetadataConformance, AProbeFindsFilesAndDirectoriesAndFollowsLinks)
    {
        using SharedData::FileType;
        dialect_->learnFrom(features_);
        ASSERT_TRUE(dialect_->probesEntries()) << "FEAT lists neither MLST nor SIZE";
        const auto plain = probeOf(pathOf("plain.txt"));
        ASSERT_NO_FATAL_FAILURE(expectEntry(plain, "plain.txt", FileType::Regular, 11u));
        EXPECT_TRUE(plain->modified) << "MLST and MDTM tell the exact time";
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(probeOf(pathOf("unreadable.txt")), "unreadable.txt", FileType::Regular, 7u)
        );
        ASSERT_NO_FATAL_FAILURE(expectEntry(probeOf(pathOf("unwritable/file.txt")), "file.txt", FileType::Regular, 7u));
        ASSERT_NO_FATAL_FAILURE(expectEntry(probeOf(pathOf("directory")), "directory", FileType::Directory));
        // MLST of proftpd and SIZE and CWD of vsftpd follow links, a dangling one reads as missing.
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(probeOf(pathOf("link-to-plain.txt")), "link-to-plain.txt", FileType::Regular)
        );
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(probeOf(pathOf("link-to-directory")), "link-to-directory", FileType::Directory)
        );
        for (auto const name : {"dangling-link", "does-not-exist", "does-not-exist/child", "plain.txt/child"})
            ASSERT_NO_FATAL_FAILURE(expectMissing(probeOf(pathOf(name)), name));
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
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

    TEST_P(MetadataConformance, MakesADirectoryOnceAndTellsWhyNot)
    {
        using RemoteIo::RemoteErrorKind;
        const auto made = makeDirectoryAt(pathOf("made"));
        ASSERT_TRUE(made) << made.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("made")), "made", SharedData::FileType::Directory));
        ASSERT_NO_FATAL_FAILURE(expectFailure(makeDirectoryAt(pathOf("made")), RemoteErrorKind::AlreadyExists, "made"));
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(makeDirectoryAt(pathOf("plain.txt")), RemoteErrorKind::AlreadyExists, "plain.txt")
        );
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(makeDirectoryAt(pathOf("missing/child")), RemoteErrorKind::NoSuchFile, "missing/child")
        );
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(makeDirectoryAt(pathOf("plain.txt/child")), RemoteErrorKind::NoSuchFile, "plain.txt/child")
        );
        ASSERT_NO_FATAL_FAILURE(expectRefusedInItsDirectory(makeDirectoryAt(pathOf("unwritable/new")), "unwritable/new"));
    }

    TEST_P(MetadataConformance, RemovesFilesAndLinksAndTellsWhyNot)
    {
        using RemoteIo::RemoteErrorKind;
        for (auto const name : {"plain.txt", "dangling-link", "link-to-directory"})
        {
            const auto removed = removeFileAt(pathOf(name));
            ASSERT_TRUE(removed) << name << ": " << removed.error().toString();
            ASSERT_NO_FATAL_FAILURE(expectMissing(lstatOf(pathOf(name)), name));
        }
        // The link went, not what it pointed to.
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(lstatOf(pathOf("directory")), "directory", SharedData::FileType::Directory)
        );
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(removeFileAt(pathOf("does-not-exist")), RemoteErrorKind::NoSuchFile, "does-not-exist")
        );
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(removeFileAt(pathOf("directory")), RemoteErrorKind::Failure, "directory")
        );
        ASSERT_NO_FATAL_FAILURE(expectFailure(
            removeFileAt(pathOf("unwritable/file.txt")), RemoteErrorKind::PermissionDenied, "unwritable/file.txt"
        ));
    }

    TEST_P(MetadataConformance, RemovesOnlyEmptyDirectoriesAndTellsWhyNot)
    {
        using RemoteIo::RemoteErrorKind;
        const auto full = removeDirectoryAt(pathOf("directory"));
        ASSERT_NO_FATAL_FAILURE(expectFailure(full, RemoteErrorKind::Failure, "directory"));
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(removeDirectoryAt(pathOf("plain.txt")), RemoteErrorKind::Failure, "plain.txt")
        );
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(removeDirectoryAt(pathOf("does-not-exist")), RemoteErrorKind::NoSuchFile, "does-not-exist")
        );
        const auto emptied = removeFileAt(pathOf("directory/nested/inside.txt"));
        ASSERT_TRUE(emptied) << emptied.error().toString();
        for (auto const name : {"directory/nested", "directory"})
        {
            const auto removed = removeDirectoryAt(pathOf(name));
            ASSERT_TRUE(removed) << name << ": " << removed.error().toString();
            ASSERT_NO_FATAL_FAILURE(expectMissing(lstatOf(pathOf(name)), name));
        }
    }

    TEST_P(MetadataConformance, RenamesReplacingAFileAndTellsWhyNot)
    {
        using RemoteIo::RemoteErrorKind;
        const auto renamed = rename(pathOf("one.bin"), pathOf("renamed.bin"));
        ASSERT_TRUE(renamed) << renamed.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectMissing(lstatOf(pathOf("one.bin")), "one.bin"));
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(lstatOf(pathOf("renamed.bin")), "renamed.bin", SharedData::FileType::Regular, 1u)
        );
        // Both servers replace a file, so their renameSemantics stays Unknown for the others.
        const auto replaced = rename(pathOf("empty.bin"), pathOf("plain.txt"));
        ASSERT_TRUE(replaced) << replaced.error().toString();
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(lstatOf(pathOf("plain.txt")), "plain.txt", SharedData::FileType::Regular, 0u)
        );
        ASSERT_NO_FATAL_FAILURE(expectFailure(
            rename(pathOf("does-not-exist"), pathOf("anything")), RemoteErrorKind::NoSuchFile, "does-not-exist"
        ));
        ASSERT_NO_FATAL_FAILURE(expectFailure(
            rename(pathOf("chunk.bin"), pathOf("missing/chunk.bin")), RemoteErrorKind::NoSuchFile, "missing/chunk.bin"
        ));
        ASSERT_NO_FATAL_FAILURE(expectRefusedInItsDirectory(
            rename(pathOf("chunk.bin"), pathOf("unwritable/chunk.bin")), "unwritable/chunk.bin"
        ));
        EXPECT_FALSE(rename(pathOf("chunk.bin"), pathOf("directory"))) << "a file replaced a directory";
        ASSERT_NO_FATAL_FAILURE(expectUsableLogin());
    }

    TEST_P(MetadataConformance, ReadsSizesThroughLinks)
    {
        using RemoteIo::RemoteErrorKind;
        using SizeResult = std::expected<std::uint64_t, RemoteIo::RemoteError>;
        const auto sizeAt = [this](std::string path)
        {
            return perform<SizeResult>(
                [path](MetadataSession session)
                {
                    return sizeOf(std::move(session), path);
                }
            );
        };
        ASSERT_NO_FATAL_FAILURE(expectValue(sizeAt(pathOf("plain.txt")), std::uint64_t{11u}, "plain.txt"));
        ASSERT_NO_FATAL_FAILURE(
            expectValue(sizeAt(pathOf("link-to-plain.txt")), std::uint64_t{11u}, "link-to-plain.txt")
        );
        ASSERT_NO_FATAL_FAILURE(expectValue(sizeAt(pathOf("unreadable.txt")), std::uint64_t{7u}, "unreadable.txt"));
        const auto missing = sizeAt(pathOf("does-not-exist"));
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().kind, RemoteErrorKind::NoSuchFile) << missing.error().toString();
        EXPECT_FALSE(sizeAt(pathOf("directory")));
    }

    TEST_P(MetadataConformance, SetsTheModificationTimeWithWhatTheServerTakes)
    {
        using namespace std::chrono;
        using RemoteIo::RemoteErrorKind;
        const auto setAt = [this](std::string path, sys_seconds time)
        {
            return perform<VoidResult>(
                [path, time](MetadataSession session)
                {
                    return setModificationTime(std::move(session), path, time);
                }
            );
        };
        const auto time = sys_seconds{sys_days{2020y / January / 2} + 3h + 4min + 5s};
        const auto set = setAt(pathOf("plain.txt"), time);
        ASSERT_TRUE(set) << set.error().toString();
        const auto stated = statOf(pathOf("plain.txt"));
        ASSERT_TRUE(stated) << stated.error().toString();
        ASSERT_TRUE(stated->modified);
        EXPECT_EQ(floor<seconds>(*stated->modified), time);

        const auto expected = [this]()
        {
            switch (profile_.expectations.modificationTime)
            {
                case ModificationTimeCommand::Mfmt:
                    return ModificationTimeSetter::ModifyFact;
                case ModificationTimeCommand::Mdtm:
                    return ModificationTimeSetter::ModificationTimeWithTime;
                case ModificationTimeCommand::SiteUtime:
                    return ModificationTimeSetter::SiteUtime;
            }
            return ModificationTimeSetter::Unknown;
        }();
        EXPECT_EQ(dialect_->modificationTimeSetter(), expected);

        ASSERT_NO_FATAL_FAILURE(
            expectFailure(setAt(pathOf("does-not-exist"), time), RemoteErrorKind::NoSuchFile, "does-not-exist")
        );
        ASSERT_NO_FATAL_FAILURE(expectFailure(
            setAt(pathOf("plain.txt"), sys_seconds{sys_days{999y / December / 31}}), RemoteErrorKind::Unsupported, "999"
        ));
    }

    TEST_P(MetadataConformance, ResolvesDirectoriesAsTheServerDoes)
    {
        using RemoteIo::RemoteErrorKind;
        using PathResult = std::expected<std::string, RemoteIo::RemoteError>;
        const auto canonical = [this](std::string path)
        {
            return perform<PathResult>(
                [path](MetadataSession session)
                {
                    return canonicalDirectory(std::move(session), path);
                }
            );
        };
        ASSERT_NO_FATAL_FAILURE(
            expectValue(canonical(pathOf("link-to-directory")), pathOf("directory"), "link-to-directory")
        );
        ASSERT_NO_FATAL_FAILURE(
            expectValue(canonical(pathOf("directory/../directory")), pathOf("directory"), "directory/../directory")
        );
        const auto file = canonical(pathOf("plain.txt"));
        ASSERT_FALSE(file);
        EXPECT_EQ(file.error().kind, RemoteErrorKind::Failure) << file.error().toString();
        const auto missing = canonical(pathOf("does-not-exist"));
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().kind, RemoteErrorKind::NoSuchFile) << missing.error().toString();
    }

    TEST_P(MetadataConformance, CreatesAnEmptyFileOnlyWhereNoneIs)
    {
        using RemoteIo::RemoteErrorKind;
        const auto create = [this](std::string path)
        {
            return perform<VoidResult>(
                [path](MetadataSession session)
                {
                    return createEmptyFile(std::move(session), path);
                }
            );
        };
        const auto created = create(pathOf("new.txt"));
        ASSERT_TRUE(created) << created.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectEntry(lstatOf(pathOf("new.txt")), "new.txt", SharedData::FileType::Regular, 0u));
        const auto kept = create(pathOf("plain.txt"));
        ASSERT_TRUE(kept) << kept.error().toString();
        ASSERT_NO_FATAL_FAILURE(
            expectEntry(lstatOf(pathOf("plain.txt")), "plain.txt", SharedData::FileType::Regular, 11u)
        );
        ASSERT_NO_FATAL_FAILURE(
            expectFailure(create(pathOf("missing/new.txt")), RemoteErrorKind::NoSuchFile, "missing/new.txt")
        );
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
