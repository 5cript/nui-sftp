#pragma once

#include <tar_archive/archive.hpp>
#include <tar_archive/entry_reader.hpp>
#include <tar_archive/reader.hpp>

#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Test
{
    /**
     * @brief Entry recovered from an archive on disk — path/type/payload. Used
     *        by the archive-operation tests to assert round-trip equality.
     */
    struct RecoveredEntry
    {
        std::string path{};
        SharedData::FileType type{};
        std::string contents{};
    };

    inline SharedData::DirectoryEntry
    makeFileEntry(std::filesystem::path const& fullPath, std::uint64_t size)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = fullPath;
        entry.fullPath = fullPath;
        entry.type = SharedData::FileType::Regular;
        entry.size = size;
        return entry;
    }

    inline SharedData::DirectoryEntry
    makeDirectoryEntry(std::filesystem::path const& fullPath)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = fullPath;
        entry.fullPath = fullPath;
        entry.type = SharedData::FileType::Directory;
        return entry;
    }

    /**
     * @brief Open the archive at @p archivePath and collect every entry's
     *        metadata + payload bytes into a vector for order-insensitive
     *        assertions in the tests.
     */
    inline std::vector<RecoveredEntry>
    recoverArchiveEntries(std::filesystem::path const& archivePath)
    {
        std::vector<RecoveredEntry> out;
        TarArchive::Archive archive{archivePath};
        auto readerOrError = archive.openReader();
        EXPECT_TRUE(readerOrError.has_value())
            << "openReader failed for archive '" << archivePath.generic_string() << "'";
        if (!readerOrError.has_value())
            return out;
        auto reader = std::move(*readerOrError);
        while (true)
        {
            auto nextResult = reader.nextEntry();
            EXPECT_TRUE(nextResult.has_value()) << "nextEntry() returned an error";
            if (!nextResult.has_value())
                break;
            if (!nextResult->has_value())
                break;
            auto entry = std::move(**nextResult);

            RecoveredEntry recovered{};
            recovered.path = entry.directoryEntry().path.generic_string();
            recovered.type = entry.directoryEntry().type;

            if (recovered.type == SharedData::FileType::Regular)
            {
                std::string payload{};
                std::array<std::byte, 4096u> buffer{};
                while (true)
                {
                    auto readResult = entry.read(std::span{buffer});
                    EXPECT_TRUE(readResult.has_value()) << "EntryReader::read failed";
                    if (!readResult.has_value())
                        break;
                    if (*readResult == 0u)
                        break;
                    payload.append(
                        reinterpret_cast<char const*>(buffer.data()),
                        *readResult
                    );
                }
                recovered.contents = std::move(payload);
            }
            out.push_back(std::move(recovered));
        }
        return out;
    }

    inline RecoveredEntry const*
    findEntry(std::vector<RecoveredEntry> const& entries, std::string_view path)
    {
        const auto it = std::find_if(
            entries.begin(), entries.end(),
            [path](RecoveredEntry const& entry) { return entry.path == path; }
        );
        return it == entries.end() ? nullptr : &*it;
    }
}
