#pragma once

#include <tar_archive/header.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

namespace TarArchive::Test
{
    namespace
    {
        SharedData::DirectoryEntry makeRegularEntry(std::string const& path, std::uint64_t size)
        {
            SharedData::DirectoryEntry entry{};
            entry.path = std::filesystem::path{path};
            entry.fullPath = entry.path;
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

    TEST(TarHeader, BuildRecordsForDirectoryHasTrailingSlashAndNoSize)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = "var/log";
        entry.fullPath = "var/log";
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
        SharedData::DirectoryEntry entry{};
        entry.path = "foo";
        entry.fullPath = "foo";
        entry.type = SharedData::FileType::Unknown;
        const auto built = buildRecords(entry);
        ASSERT_FALSE(built.has_value());
        EXPECT_EQ(built.error().code, TarErrorCode::UnsupportedFileType);
    }

    TEST(TarHeader, SymlinkPopulatesLinkNameField)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = "link";
        entry.fullPath = "link";
        entry.type = SharedData::FileType::Symlink;
        entry.linkTarget = std::filesystem::path{"target.txt"};
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

    TEST(TarHeader, LargeSizeTriggersPaxExtendedHeader)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = "big.bin";
        entry.fullPath = "big.bin";
        entry.type = SharedData::FileType::Regular;
        entry.size = 1ull << 34;
        entry.permissions = static_cast<std::filesystem::perms>(0644u);

        const auto built = buildRecords(entry);
        ASSERT_TRUE(built.has_value());
        EXPECT_GT(built->records.size(), 1u);
    }
}
