#pragma once

#include <tar_archive/archive.hpp>
#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

extern std::filesystem::path programDirectory;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path gnuInteropDirectory()
        {
            const auto directory = programDirectory / "temp" / "tar_archive_gnu_interop";
            std::filesystem::create_directories(directory);
            return directory;
        }

        void writeFile(std::filesystem::path const& path, std::span<std::byte const> bytes)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(
                reinterpret_cast<char const*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
        }

        std::vector<std::byte> readFile(std::filesystem::path const& path)
        {
            std::ifstream stream{path, std::ios::binary | std::ios::ate};
            const auto size = static_cast<std::size_t>(stream.tellg());
            stream.seekg(0);
            std::vector<std::byte> bytes(size);
            stream.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(size)
            );
            return bytes;
        }

        std::vector<std::byte> deterministicPattern(std::size_t size, std::uint32_t seed)
        {
            std::vector<std::byte> payload(size);
            std::uint32_t state = seed | 1u;
            for (std::size_t position = 0u; position < size; ++position)
            {
                state = state * 1103515245u + 12345u;
                payload[position] = static_cast<std::byte>((state >> 16u) & 0xFFu);
            }
            return payload;
        }

        /**
         * @brief Run a command via the shell with stdout redirected to @p outputPath.
         * Returns the child's exit status (0 on success).
         */
        int runShell(std::string const& command, std::filesystem::path const& outputPath)
        {
            const std::string full = command + " > " + outputPath.string() + " 2>&1";
            return std::system(full.c_str());
        }

        SharedData::DirectoryEntry regularEntryForPath(
            std::string const& archiveName, std::uint64_t size
        )
        {
            SharedData::DirectoryEntry entry{};
            entry.path = std::filesystem::path{archiveName};
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

    /**
     * @brief Suite-level precheck: if TAR_EXECUTABLE is empty or does not point at an
     * existing file, every test in the suite fails in SetUp with a clear message.
     */
    class TarGnuInterop : public ::testing::Test
    {
      protected:
        static inline std::string tarBinary_{};
        static inline bool tarChecked_{false};
        static inline bool tarAvailable_{false};

        static void SetUpTestSuite()
        {
            tarBinary_ = std::string{TAR_EXECUTABLE};
            tarChecked_ = true;
            tarAvailable_ =
                !tarBinary_.empty() && std::filesystem::exists(tarBinary_);
        }

        void SetUp() override
        {
            if (!tarChecked_)
                SetUpTestSuite();
            if (!tarAvailable_)
                FAIL()
                    << "system 'tar' binary not found (TAR_EXECUTABLE='"
                    << tarBinary_
                    << "'); install GNU tar to run these interop tests";
        }
    };

    TEST_F(TarGnuInterop, LibraryProducedArchiveIsListableByTar)
    {
        const auto directory = gnuInteropDirectory();
        const auto archivePath = directory / "library_produced.tar";
        const auto listOutput = directory / "library_produced.list";

        const std::vector<std::string> names{
            "readme.txt", "data/alpha.bin", "data/beta.bin"
        };

        Archive archive{archivePath};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            for (auto const& name : names)
            {
                const auto payload = deterministicPattern(1024u, 0xABCDu);
                auto entry = writer->beginEntry(regularEntryForPath(name, payload.size()));
                ASSERT_TRUE(entry.has_value());
                ASSERT_TRUE(entry->write(std::span<std::byte const>{payload}).has_value());
                ASSERT_TRUE(std::move(*entry).close().has_value());
            }
            ASSERT_TRUE(writer->finalize().has_value());
        }

        const int status = runShell(
            tarBinary_ + " -tf '" + archivePath.string() + "'", listOutput
        );
        ASSERT_EQ(status, 0) << "tar -tf exited with non-zero status";

        std::ifstream stream{listOutput};
        std::vector<std::string> listed;
        std::string line;
        while (std::getline(stream, line))
            listed.push_back(line);
        EXPECT_EQ(listed, names);
    }

    TEST_F(TarGnuInterop, LibraryProducedContentMatchesTarExtraction)
    {
        const auto directory = gnuInteropDirectory();
        const auto archivePath = directory / "library_content.tar";
        const auto extractDir = directory / "extracted";
        std::filesystem::remove_all(extractDir);
        std::filesystem::create_directories(extractDir);

        const std::string entryName = "checked.bin";
        const auto payload = deterministicPattern(4097u, 0xBEEFu);

        Archive archive{archivePath};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            auto entry = writer->beginEntry(regularEntryForPath(entryName, payload.size()));
            ASSERT_TRUE(entry.has_value());
            ASSERT_TRUE(entry->write(std::span<std::byte const>{payload}).has_value());
            ASSERT_TRUE(std::move(*entry).close().has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }

        const std::string command =
            tarBinary_ + " -xf '" + archivePath.string() +
            "' -C '" + extractDir.string() + "'";
        const int status = runShell(command, directory / "library_content.log");
        ASSERT_EQ(status, 0) << "tar -xf exited with non-zero status";

        const auto extracted = readFile(extractDir / entryName);
        EXPECT_EQ(extracted, payload);
    }

    TEST_F(TarGnuInterop, LibraryReadsTarProducedArchive)
    {
        const auto directory = gnuInteropDirectory();
        const auto sourceTree = directory / "source_tree";
        std::filesystem::remove_all(sourceTree);
        std::filesystem::create_directories(sourceTree);

        struct Fixture
        {
            std::string name;
            std::vector<std::byte> payload;
        };
        const std::array<Fixture, 5u> fixtures{{
            {"zero.bin", deterministicPattern(0u, 0x1u)},
            {"exactly_one_record.bin", deterministicPattern(512u, 0x2u)},
            {"one_record_plus_one.bin", deterministicPattern(513u, 0x3u)},
            {"eight_k.bin", deterministicPattern(8192u, 0x4u)},
            {"eight_k_plus_one.bin", deterministicPattern(8193u, 0x5u)},
        }};

        for (auto const& fixture : fixtures)
            writeFile(sourceTree / fixture.name, fixture.payload);

        const auto archivePath = directory / "tar_produced.tar";

        std::string createCommand = tarBinary_ + " -cf '" + archivePath.string() + "' -C '" +
                                    sourceTree.string() + "'";
        for (auto const& fixture : fixtures)
            createCommand += " " + fixture.name;

        const int status = runShell(createCommand, directory / "tar_produced.log");
        ASSERT_EQ(status, 0) << "tar -cf exited with non-zero status";

        Archive archive{archivePath};
        auto reader = archive.openReader();
        ASSERT_TRUE(reader.has_value()) << reader.error().toString();

        for (auto const& fixture : fixtures)
        {
            auto next = reader->nextEntry();
            ASSERT_TRUE(next.has_value()) << next.error().toString();
            ASSERT_TRUE(next->has_value()) << "premature end of archive";

            auto entry = std::move(**next);
            EXPECT_EQ(entry.directoryEntry().path.generic_string(), fixture.name);
            EXPECT_EQ(entry.directoryEntry().size, fixture.payload.size());

            std::vector<std::byte> accumulator;
            std::array<std::byte, 1024u> scratch{};
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
            EXPECT_EQ(accumulator, fixture.payload)
                << "payload mismatch for " << fixture.name;
        }

        const auto terminator = reader->nextEntry();
        ASSERT_TRUE(terminator.has_value());
        EXPECT_FALSE(terminator->has_value());
    }

    TEST_F(TarGnuInterop, LibraryProducedGzipArchiveIsReadableByTar)
    {
        const auto directory = gnuInteropDirectory();
        const auto archivePath = directory / "library_produced.tar.gz";
        const auto listOutput = directory / "library_produced_gz.list";

        const auto payload = deterministicPattern(9000u, 0xC0DEu);

        Archive archive{archivePath};
        {
            auto writer = archive.openWriter();
            ASSERT_TRUE(writer.has_value());
            auto entry = writer->beginEntry(regularEntryForPath("inside.bin", payload.size()));
            ASSERT_TRUE(entry.has_value());
            ASSERT_TRUE(entry->write(std::span<std::byte const>{payload}).has_value());
            ASSERT_TRUE(std::move(*entry).close().has_value());
            ASSERT_TRUE(writer->finalize().has_value());
        }

        const int status = runShell(
            tarBinary_ + " -tzf '" + archivePath.string() + "'", listOutput
        );
        ASSERT_EQ(status, 0) << "tar -tzf exited with non-zero status";

        std::ifstream stream{listOutput};
        std::string line;
        std::getline(stream, line);
        EXPECT_EQ(line, "inside.bin");
    }
}
