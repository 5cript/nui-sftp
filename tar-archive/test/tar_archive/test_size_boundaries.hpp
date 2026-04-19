#pragma once

#include <tar_archive/archive.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

extern std::filesystem::path programDirectory;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path sizeBoundaryDirectory()
        {
            const auto directory = programDirectory / "temp" / "tar_archive_sizes";
            std::filesystem::create_directories(directory);
            return directory;
        }

        std::vector<std::byte> deterministicBytes(std::size_t size)
        {
            std::vector<std::byte> payload(size);
            for (std::size_t position = 0u; position < size; ++position)
                payload[position] = static_cast<std::byte>((position * 37u + 11u) & 0xFFu);
            return payload;
        }

        SharedData::DirectoryEntry regularAtSize(std::uint64_t size)
        {
            SharedData::DirectoryEntry entry{};
            entry.path = "payload.bin";
            entry.fullPath = "payload.bin";
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

    class TarSizeBoundary : public ::testing::TestWithParam<std::size_t>
    {};

    TEST_P(TarSizeBoundary, WriterReaderRoundTripAtBoundarySizes)
    {
        const std::size_t size = GetParam();
        const auto path =
            sizeBoundaryDirectory() / ("size_" + std::to_string(size) + ".tar");
        const auto payload = deterministicBytes(size);

        Archive archive{path};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value()) << writer.error().toString();
            auto entry = writer->beginEntry(regularAtSize(size));
            ASSERT_TRUE(entry.has_value()) << entry.error().toString();
            if (size > 0u)
                ASSERT_TRUE(entry->write(std::span<std::byte const>{payload}).has_value());
            ASSERT_TRUE(std::move(*entry).close().has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }

        auto reader = archive.openReader();
        ASSERT_TRUE(reader.has_value()) << reader.error().toString();

        auto next = reader->nextEntry();
        ASSERT_TRUE(next.has_value());
        ASSERT_TRUE(next->has_value());

        auto entry = std::move(**next);
        EXPECT_EQ(entry.size(), size);

        std::vector<std::byte> accumulator;
        std::array<std::byte, 256u> scratch{};
        while (true)
        {
            const auto produced = entry.read(scratch);
            ASSERT_TRUE(produced.has_value()) << produced.error().toString();
            if (*produced == 0u)
                break;
            accumulator.insert(
                accumulator.end(),
                scratch.begin(),
                scratch.begin() + static_cast<std::ptrdiff_t>(*produced)
            );
        }
        EXPECT_EQ(accumulator.size(), size);
        EXPECT_EQ(accumulator, payload);

        const auto terminator = reader->nextEntry();
        ASSERT_TRUE(terminator.has_value());
        EXPECT_FALSE(terminator->has_value());
    }

    INSTANTIATE_TEST_SUITE_P(
        RecordBoundaries,
        TarSizeBoundary,
        ::testing::Values(
            std::size_t{0u},
            std::size_t{1u},
            std::size_t{2u},
            std::size_t{511u},
            std::size_t{512u},
            std::size_t{513u},
            std::size_t{514u},
            std::size_t{1023u},
            std::size_t{1024u},
            std::size_t{1025u},
            std::size_t{4095u},
            std::size_t{4096u},
            std::size_t{4097u},
            std::size_t{65535u},
            std::size_t{65536u},
            std::size_t{65537u}
        ),
        [](::testing::TestParamInfo<std::size_t> const& info) {
            return "Size" + std::to_string(info.param);
        }
    );
}
