#pragma once

#include <tar_archive/header.hpp>
#include <shared_data/directory_entry.hpp>
#include <paths/utf8.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace TarArchive::Test
{
    namespace
    {
        TarArchive::MemberHeader makeRegularEntry(std::string const& path, std::uint64_t size)
        {
            TarArchive::MemberHeader entry{};
            entry.path = path;
            entry.type = SharedData::FileType::Regular;
            entry.size = size;
            entry.permissions = static_cast<std::filesystem::perms>(0644u);
            entry.uid = 1000u;
            entry.gid = 1000u;
            entry.owner = "alice";
            entry.group = "alice";
            entry.mtime = 1700000000u;
            return entry;
        }
    }

    TEST(TarHeader, ChecksumOfZeroRecordEqualsEightSpaces)
    {
        RawRecord record{};
        const auto checksum = calculateChecksum(record);
        EXPECT_EQ(checksum, 8u * static_cast<std::uint32_t>(' '));
    }

    TEST(TarHeader, IsZeroRecordDetectsZeroesAndNonZeroes)
    {
        RawRecord zeros{};
        EXPECT_TRUE(isZeroRecord(zeros));
        zeros[42] = std::byte{0x01u};
        EXPECT_FALSE(isZeroRecord(zeros));
    }

    TEST(TarHeader, BuildRecordsForShortRegularProducesOneRecord)
    {
        const auto entry = makeRegularEntry("hello.txt", 11u);
        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value()) << built.error().toString();
        ASSERT_EQ(built->records.size(), 1u);
        EXPECT_EQ(built->payloadSize, 11u);

        const auto parsed = parseRecord(built->records[0]);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().toString();
        EXPECT_EQ(parsed->fullName(), "hello.txt");
        EXPECT_EQ(parsed->size, 11u);
        EXPECT_EQ(parsed->typeflag, TypeFlag::RegularFile);
        EXPECT_TRUE(parsed->hasUstarMagic);
        EXPECT_EQ(parsed->uid, 1000u);
        EXPECT_EQ(parsed->uName, "alice");
    }

    TEST(TarHeader, UnknownPermissionsGetTheModeOfANewMember)
    {
        const auto modeOf = [](SharedData::FileType type)
        {
            TarArchive::MemberHeader entry{};
            entry.path = "member";
            entry.type = type;
            if (type == SharedData::FileType::Symlink)
                entry.linkTarget = "target";
            const auto built = buildRecords(entry);
            EXPECT_TRUE(built.has_value());
            const auto parsed = parseRecord(built->records.back());
            EXPECT_TRUE(parsed.has_value());
            return static_cast<std::uint32_t>(parsed->permissions);
        };
        EXPECT_EQ(modeOf(SharedData::FileType::Regular), 0644u);
        EXPECT_EQ(modeOf(SharedData::FileType::Directory), 0755u);
        EXPECT_EQ(modeOf(SharedData::FileType::Symlink), 0777u);
    }

    TEST(TarHeader, BuildRecordsForDirectoryHasTrailingSlashAndNoSize)
    {
        TarArchive::MemberHeader entry{};
        entry.path = "var/log";
        entry.type = SharedData::FileType::Directory;
        entry.permissions = static_cast<std::filesystem::perms>(0755u);

        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value()) << built.error().toString();
        ASSERT_EQ(built->records.size(), 1u);
        EXPECT_EQ(built->payloadSize, 0u);

        const auto parsed = parseRecord(built->records[0]);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->typeflag, TypeFlag::Directory);
        EXPECT_EQ(parsed->fullName(), "var/log/");
        EXPECT_EQ(parsed->size, 0u);
    }

    TEST(TarHeader, BuildRecordsSplitsPrefixForLongishPath)
    {
        const std::string longPath =
            std::string(80u, 'a') + "/" + std::string(80u, 'b');
        const auto entry = makeRegularEntry(longPath, 0u);

        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value());
        ASSERT_EQ(built->records.size(), 1u);

        const auto parsed = parseRecord(built->records[0]);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->fullName(), longPath);
        EXPECT_FALSE(parsed->prefix.empty());
    }

    TEST(TarHeader, ParseRecordDetectsChecksumMismatch)
    {
        const auto entry = makeRegularEntry("x.txt", 0u);
        auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value());
        RawRecord& record = built->records[0];
        record[10] = static_cast<std::byte>(static_cast<std::uint8_t>(record[10]) ^ 0xFFu);
        const auto parsed = parseRecord(record);
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().code, TarErrorCode::ChecksumMismatch);
    }

    TEST(TarHeader, RejectsUnsupportedFileType)
    {
        TarArchive::MemberHeader entry{};
        entry.path = "foo";
        entry.type = SharedData::FileType::Unknown;
        const auto built = buildRecords(entry);
        ASSERT_FALSE(built.has_value());
        EXPECT_EQ(built.error().code, TarErrorCode::UnsupportedFileType);
    }

    TEST(TarHeader, SymlinkPopulatesLinkNameField)
    {
        TarArchive::MemberHeader entry{};
        entry.path = "link";
        entry.type = SharedData::FileType::Symlink;
        entry.linkTarget = "target.txt";
        entry.permissions = static_cast<std::filesystem::perms>(0777u);

        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value());
        ASSERT_EQ(built->records.size(), 1u);

        const auto parsed = parseRecord(built->records[0]);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->typeflag, TypeFlag::SymbolicLink);
        EXPECT_EQ(parsed->linkName, "target.txt");
    }

    TEST(TarHeader, TypeFlagRoundTripThroughFileType)
    {
        using SharedData::FileType;
        EXPECT_EQ(fileTypeFromTypeFlag(TypeFlag::RegularFile), FileType::Regular);
        EXPECT_EQ(fileTypeFromTypeFlag(TypeFlag::Directory), FileType::Directory);
        EXPECT_EQ(fileTypeFromTypeFlag(TypeFlag::SymbolicLink), FileType::Symlink);
        EXPECT_EQ(fileTypeFromTypeFlag(TypeFlag::CharacterSpecial), FileType::CharDevice);
        EXPECT_EQ(fileTypeFromTypeFlag(TypeFlag::BlockSpecial), FileType::BlockDevice);
        EXPECT_EQ(fileTypeFromTypeFlag(TypeFlag::FifoSpecial), FileType::Fifo);
    }

    TEST(TarHeader, NonAsciiNamesGetPaxRecords)
    {
        const std::string name = "Grüße/東京.txt";
        const auto built = buildRecords(makeRegularEntry(name, 4u));
        ASSERT_TRUE(built.has_value()) << built.error().toString();
        ASSERT_EQ(built->records.size(), 3u);

        const auto paxHeader = parseRecord(built->records[0]);
        ASSERT_TRUE(paxHeader.has_value());
        EXPECT_EQ(paxHeader->typeflag, TypeFlag::ExtendedHeader);
        const auto overrides = parsePaxPayload(
            std::span<std::byte const>{built->records[1]}.first(static_cast<std::size_t>(paxHeader->size))
        );
        ASSERT_TRUE(overrides.has_value()) << overrides.error().toString();
        EXPECT_EQ(overrides->path, name);

        const auto main = parseRecord(built->records[2]);
        ASSERT_TRUE(main.has_value());
        EXPECT_EQ(main->fullName(), name);
    }

    TEST(TarHeader, NonAsciiLinkTargetsGetPaxRecords)
    {
        TarArchive::MemberHeader entry{};
        entry.path = "link";
        entry.type = SharedData::FileType::Symlink;
        entry.linkTarget = "Ärger.txt";
        entry.permissions = static_cast<std::filesystem::perms>(0777u);

        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value());
        ASSERT_EQ(built->records.size(), 3u);
        const auto paxHeader = parseRecord(built->records[0]);
        ASSERT_TRUE(paxHeader.has_value());
        const auto overrides = parsePaxPayload(
            std::span<std::byte const>{built->records[1]}.first(static_cast<std::size_t>(paxHeader->size))
        );
        ASSERT_TRUE(overrides.has_value());
        EXPECT_EQ(overrides->linkPath, "Ärger.txt");
        EXPECT_FALSE(overrides->path.has_value());
    }

    TEST(TarHeader, TruncatedUstarNamesEndAtACodePointBoundary)
    {
        std::string name;
        for (int index = 0; index < 120; ++index)
            name += "ü";
        const auto built = buildRecords(makeRegularEntry(name, 1u));
        ASSERT_TRUE(built.has_value());
        const auto main = parseRecord(built->records.back());
        ASSERT_TRUE(main.has_value());
        EXPECT_EQ(main->fullName().size(), 100u);
        EXPECT_TRUE(Paths::isValidUtf8(main->fullName()));

        const auto paxHeader = parseRecord(built->records.front());
        ASSERT_TRUE(paxHeader.has_value());
        EXPECT_TRUE(Paths::isValidUtf8(paxHeader->fullName()));
    }

    TEST(TarHeader, LargeSizeTriggersPaxExtendedHeader)
    {
        TarArchive::MemberHeader entry{};
        entry.path = "big.bin";
        entry.type = SharedData::FileType::Regular;
        entry.size = 1ull << 34;
        entry.permissions = static_cast<std::filesystem::perms>(0644u);

        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value());
        EXPECT_GT(built->records.size(), 1u);
    }
}
