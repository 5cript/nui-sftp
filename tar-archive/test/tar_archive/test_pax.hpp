#pragma once

#include <tar_archive/archive.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

extern std::filesystem::path programDirectory;
extern std::filesystem::path temporaryRoot;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path paxDirectory()
        {
            const auto directory = temporaryRoot / "tar_archive_pax";
            std::filesystem::create_directories(directory);
            return directory;
        }
    }

    TEST(TarPax, VeryLongPathRoundTripsViaExtendedHeader)
    {
        const std::string longName = std::string(180u, 'a') + "/" + std::string(200u, 'b') + "/file.bin";
        const std::string contents = "payload for long path entry";

        TarArchive::MemberHeader meta{};
        meta.path = longName;
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
        EXPECT_EQ(entry.header().path, longName);
        EXPECT_EQ(entry.header().size, contents.size());
    }

    TEST(TarPax, LongSymlinkTargetRoundTripsViaExtendedHeader)
    {
        const std::string linkText = std::string(250u, 'x');
        TarArchive::MemberHeader meta{};
        meta.path = "link";
        meta.type = SharedData::FileType::Symlink;
        meta.linkTarget = linkText;
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
        EXPECT_EQ(entry.header().type, SharedData::FileType::Symlink);
        ASSERT_TRUE(entry.header().linkTarget.has_value());
        EXPECT_EQ(*entry.header().linkTarget, linkText);
    }
}
