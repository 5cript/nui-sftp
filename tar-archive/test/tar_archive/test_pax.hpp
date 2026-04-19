#pragma once

#include <tar_archive/archive.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

extern std::filesystem::path programDirectory;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path paxDirectory()
        {
            const auto directory = programDirectory / "temp" / "tar_archive_pax";
            std::filesystem::create_directories(directory);
            return directory;
        }
    }

    TEST(TarPax, VeryLongPathRoundTripsViaExtendedHeader)
    {
        const std::string longName = std::string(180u, 'a') + "/" + std::string(200u, 'b') + "/file.bin";
        const std::string contents = "payload for long path entry";

        SharedData::DirectoryEntry meta{};
        meta.path = std::filesystem::path{longName};
        meta.fullPath = meta.path;
        meta.type = SharedData::FileType::Regular;
        meta.size = contents.size();
        meta.permissions = static_cast<std::filesystem::perms>(0644u);
        meta.owner = "alice";
        meta.group = "alice";

        const auto path = paxDirectory() / "longname.tar";
        Archive archive{path};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            auto entry = writer->beginEntry(meta);
            ASSERT_TRUE(entry.has_value());
            std::vector<std::byte> bytes(contents.size());
            for (std::size_t position = 0u; position < contents.size(); ++position)
                bytes[position] = static_cast<std::byte>(contents[position]);
            ASSERT_TRUE(entry->write(std::span<std::byte const>{bytes}).has_value());
            ASSERT_TRUE(std::move(*entry).close().has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }

        auto reader = archive.openReader();
        ASSERT_TRUE(reader.has_value());
        auto next = reader->nextEntry();
        ASSERT_TRUE(next.has_value());
        ASSERT_TRUE(next->has_value());
        auto entry = std::move(**next);
        EXPECT_EQ(entry.directoryEntry().path.generic_string(), longName);
        EXPECT_EQ(entry.directoryEntry().size, contents.size());
    }

    TEST(TarPax, LongSymlinkTargetRoundTripsViaExtendedHeader)
    {
        const std::string linkText = std::string(250u, 'x');
        SharedData::DirectoryEntry meta{};
        meta.path = "link";
        meta.fullPath = "link";
        meta.type = SharedData::FileType::Symlink;
        meta.linkTarget = std::filesystem::path{linkText};
        meta.permissions = static_cast<std::filesystem::perms>(0777u);

        const auto path = paxDirectory() / "longlink.tar";
        Archive archive{path};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            auto entry = writer->beginEntry(meta);
            ASSERT_TRUE(entry.has_value());
            ASSERT_TRUE(std::move(*entry).close().has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }

        auto reader = archive.openReader();
        ASSERT_TRUE(reader.has_value());
        auto next = reader->nextEntry();
        ASSERT_TRUE(next.has_value());
        ASSERT_TRUE(next->has_value());
        auto entry = std::move(**next);
        EXPECT_EQ(entry.directoryEntry().type, SharedData::FileType::Symlink);
        ASSERT_TRUE(entry.directoryEntry().linkTarget.has_value());
        EXPECT_EQ(entry.directoryEntry().linkTarget->generic_string(), linkText);
    }
}
