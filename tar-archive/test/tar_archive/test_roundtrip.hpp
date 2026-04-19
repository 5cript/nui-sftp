#pragma once

#include <tar_archive/archive.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

extern std::filesystem::path programDirectory;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path roundTripDirectory()
        {
            const auto directory = programDirectory / "temp" / "tar_archive_roundtrip";
            std::filesystem::create_directories(directory);
            return directory;
        }

        SharedData::DirectoryEntry regular(std::string const& path, std::uint64_t size)
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

        SharedData::DirectoryEntry directory(std::string const& path)
        {
            SharedData::DirectoryEntry entry{};
            entry.path = std::filesystem::path{path};
            entry.fullPath = entry.path;
            entry.type = SharedData::FileType::Directory;
            entry.permissions = static_cast<std::filesystem::perms>(0755u);
            entry.owner = "alice";
            entry.group = "alice";
            return entry;
        }

        std::vector<std::byte> randomBytes(std::size_t size, std::uint64_t seed)
        {
            std::mt19937_64 engine{seed};
            std::vector<std::byte> payload(size);
            for (auto& datum : payload)
                datum = static_cast<std::byte>(engine() & 0xFFu);
            return payload;
        }

        struct WrittenEntry
        {
            SharedData::DirectoryEntry meta;
            std::vector<std::byte> payload;
        };

        std::expected<void, TarError>
        writeEntry(Writer& writer, WrittenEntry const& entry)
        {
            auto handle = writer.beginEntry(entry.meta);
            if (!handle)
                return std::unexpected(handle.error());
            const auto written = handle->write(std::span<std::byte const>{entry.payload});
            if (!written)
                return std::unexpected(written.error());
            return std::move(*handle).close();
        }

        std::expected<std::vector<std::byte>, TarError> readPayload(EntryReader& entry)
        {
            std::vector<std::byte> result;
            std::array<std::byte, 4096u> scratch{};
            while (true)
            {
                const auto produced = entry.read(scratch);
                if (!produced)
                    return std::unexpected(produced.error());
                if (*produced == 0u)
                    break;
                result.insert(
                    result.end(),
                    scratch.begin(),
                    scratch.begin() + static_cast<std::ptrdiff_t>(*produced)
                );
            }
            return result;
        }

        void runFullRoundTrip(std::string const& filename)
        {
            const auto path = roundTripDirectory() / filename;

            const std::vector<WrittenEntry> entries{
                {regular("readme.txt", 13u), randomBytes(13u, 0x1u)},
                {directory("src"), {}},
                {regular("src/main.cpp", 8192u), randomBytes(8192u, 0x2u)},
                {regular("src/util.hpp", 321u), randomBytes(321u, 0x3u)},
            };

            Archive archive{path};
            {
                auto writer = archive.openWriter();
                ASSERT_TRUE(writer.has_value()) << writer.error().toString();
                for (auto const& entry : entries)
                    ASSERT_TRUE(writeEntry(*writer, entry).has_value())
                        << "failed on " << entry.meta.path.string();
                ASSERT_TRUE(writer->finalize().has_value());
            }

            auto reader = archive.openReader();
            ASSERT_TRUE(reader.has_value()) << reader.error().toString();

            for (auto const& expected : entries)
            {
                auto next = reader->nextEntry();
                ASSERT_TRUE(next.has_value()) << next.error().toString();
                ASSERT_TRUE(next->has_value());
                auto entry = std::move(**next);
                EXPECT_EQ(entry.directoryEntry().path, expected.meta.path);
                EXPECT_EQ(entry.directoryEntry().type, expected.meta.type);
                EXPECT_EQ(entry.directoryEntry().size, expected.meta.size);
                EXPECT_EQ(entry.directoryEntry().owner, expected.meta.owner);
                if (expected.meta.type == SharedData::FileType::Regular)
                {
                    const auto payload = readPayload(entry);
                    ASSERT_TRUE(payload.has_value());
                    EXPECT_EQ(*payload, expected.payload);
                }
            }

            const auto terminator = reader->nextEntry();
            ASSERT_TRUE(terminator.has_value());
            EXPECT_FALSE(terminator->has_value());
        }
    }

    TEST(TarRoundTrip, PlainTarArchive)
    {
        runFullRoundTrip("trip.tar");
    }

    TEST(TarRoundTrip, GzippedArchive)
    {
        runFullRoundTrip("trip.tar.gz");
    }

    TEST(TarRoundTrip, Bzip2Archive)
    {
        runFullRoundTrip("trip.tar.bz2");
    }

    TEST(TarRoundTrip, ZstdArchive)
    {
        runFullRoundTrip("trip.tar.zst");
    }

#ifdef TAR_ARCHIVE_WITH_XZ
    TEST(TarRoundTrip, XzArchive)
    {
        runFullRoundTrip("trip.tar.xz");
    }
#endif

    TEST(TarRoundTrip, EmptyArchiveProducesValidTerminator)
    {
        const auto path = roundTripDirectory() / "empty.tar";
        Archive archive{path};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }
        auto reader = archive.openReader();
        ASSERT_TRUE(reader.has_value());
        const auto next = reader->nextEntry();
        ASSERT_TRUE(next.has_value());
        EXPECT_FALSE(next->has_value());
    }
}
