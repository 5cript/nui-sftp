#pragma once

#include <tar_archive/archive.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

extern std::filesystem::path programDirectory;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path misuseDirectory()
        {
            const auto directory = programDirectory / "temp" / "tar_archive_misuse";
            std::filesystem::create_directories(directory);
            return directory;
        }

        SharedData::DirectoryEntry smallRegular(std::uint64_t size)
        {
            SharedData::DirectoryEntry entry{};
            entry.path = "x.bin";
            entry.fullPath = "x.bin";
            entry.type = SharedData::FileType::Regular;
            entry.size = size;
            entry.permissions = static_cast<std::filesystem::perms>(0644u);
            return entry;
        }

        std::array<std::byte, 8u> eightBytes()
        {
            std::array<std::byte, 8u> bytes{};
            for (std::uint8_t index = 0u; index < bytes.size(); ++index)
                bytes[index] = std::byte{index};
            return bytes;
        }
    }

    TEST(TarMisuse, OpenReaderFailsForMissingFile)
    {
        Archive archive{misuseDirectory() / "does_not_exist.tar"};
        const auto reader = archive.openReader();
        ASSERT_FALSE(reader.has_value());
        EXPECT_EQ(reader.error().code, TarErrorCode::FileNotFound);
    }

    TEST(TarMisuse, WriteMoreThanDeclaredReturnsOverrun)
    {
        const auto path = misuseDirectory() / "overrun.tar";
        Archive archive{path};
        auto writer = archive.openWriter();
        ASSERT_TRUE(writer.has_value());
        auto entry = writer->beginEntry(smallRegular(4u));
        ASSERT_TRUE(entry.has_value());
        const auto bytes = eightBytes();
        const auto written = entry->write(std::span<std::byte const>{bytes});
        ASSERT_FALSE(written.has_value());
        EXPECT_EQ(written.error().code, TarErrorCode::OverrunOnWrite);
    }

    TEST(TarMisuse, CloseBeforeDeclaredReturnsUnderrun)
    {
        const auto path = misuseDirectory() / "underrun.tar";
        Archive archive{path};
        auto writer = archive.openWriter();
        ASSERT_TRUE(writer.has_value());
        auto entry = writer->beginEntry(smallRegular(10u));
        ASSERT_TRUE(entry.has_value());
        const auto closed = std::move(*entry).close();
        ASSERT_FALSE(closed.has_value());
        EXPECT_EQ(closed.error().code, TarErrorCode::UnderrunOnClose);
    }

    TEST(TarMisuse, SecondBeginEntryWhileOpenReturnsEntryStillOpen)
    {
        const auto path = misuseDirectory() / "conflict.tar";
        Archive archive{path};
        auto writer = archive.openWriter();
        ASSERT_TRUE(writer.has_value());
        auto first = writer->beginEntry(smallRegular(0u));
        ASSERT_TRUE(first.has_value());
        const auto second = writer->beginEntry(smallRegular(0u));
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error().code, TarErrorCode::EntryStillOpen);
    }

    TEST(TarMisuse, DoubleCloseReturnsAlreadyClosed)
    {
        const auto path = misuseDirectory() / "doubleclose.tar";
        Archive archive{path};
        auto writer = archive.openWriter();
        ASSERT_TRUE(writer.has_value());
        auto entry = writer->beginEntry(smallRegular(0u));
        ASSERT_TRUE(entry.has_value());
        EntryWriter handle = std::move(*entry);
        ASSERT_TRUE(std::move(handle).close().has_value());
        const auto closedAgain = std::move(handle).close();
        ASSERT_FALSE(closedAgain.has_value());
        EXPECT_EQ(closedAgain.error().code, TarErrorCode::AlreadyClosed);
    }

    TEST(TarMisuse, NextEntryWithoutDrainReturnsEntryStillOpen)
    {
        const auto path = misuseDirectory() / "still_open.tar";
        const std::array<std::byte, 16u> bytes{};
        {
            Archive archive{path};
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            auto entry = writer->beginEntry(smallRegular(16u));
            ASSERT_TRUE(entry.has_value());
            ASSERT_TRUE(entry->write(std::span<std::byte const>{bytes}).has_value());
            ASSERT_TRUE(std::move(*entry).close().has_value());

            SharedData::DirectoryEntry second = smallRegular(0u);
            second.path = "second.bin";
            second.fullPath = "second.bin";
            auto secondEntry = writer->beginEntry(second);
            ASSERT_TRUE(secondEntry.has_value());
            ASSERT_TRUE(std::move(*secondEntry).close().has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }

        Archive archive{path};
        auto reader = archive.openReader();
        ASSERT_TRUE(reader.has_value());

        auto first = reader->nextEntry();
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(first->has_value());

        const auto blocked = reader->nextEntry();
        ASSERT_FALSE(blocked.has_value());
        EXPECT_EQ(blocked.error().code, TarErrorCode::EntryStillOpen);

        ASSERT_TRUE(first->value().skip().has_value());
        const auto resumed = reader->nextEntry();
        ASSERT_TRUE(resumed.has_value());
        ASSERT_TRUE(resumed->has_value());
    }

    TEST(TarMisuse, MagicMismatchDetectedWhenExtensionLies)
    {
        const auto directory = misuseDirectory();
        const auto truePath = directory / "genuine.tar.gz";
        const auto mislabelled = directory / "mislabelled.tar.zst";

        Archive archive{truePath};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }
        std::filesystem::copy_file(
            truePath, mislabelled, std::filesystem::copy_options::overwrite_existing
        );

        Archive mislabelledArchive{mislabelled};
        const auto reader = mislabelledArchive.openReader();
        ASSERT_FALSE(reader.has_value());
        EXPECT_EQ(reader.error().code, TarErrorCode::MagicMismatch);
    }

    TEST(TarMisuse, FinalizeAfterFinalizeReportsAlreadyFinalized)
    {
        const auto path = misuseDirectory() / "double_finalize.tar";
        Archive archive{path};
        auto writer = archive.openWriter();
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->finalize().has_value());
        const auto again = writer->finalize();
        ASSERT_FALSE(again.has_value());
        EXPECT_EQ(again.error().code, TarErrorCode::AlreadyFinalized);
    }

    TEST(TarMisuse, WriteAfterFinalizeIsRejected)
    {
        const auto path = misuseDirectory() / "post_finalize.tar";
        Archive archive{path};
        auto writer = archive.openWriter();
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->finalize().has_value());
        const auto entry = writer->beginEntry(smallRegular(0u));
        ASSERT_FALSE(entry.has_value());
        EXPECT_EQ(entry.error().code, TarErrorCode::WriteAfterFinalize);
    }
}
