#pragma once

#include <backend/sftp/archive_download_operation.hpp>
#include <backend/sftp/archive_upload_operation.hpp>
#include <backend/sftp/bulk_download_operation.hpp>
#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/delete_operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <backend/sftp/rename_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include "remote_protocol_tests.hpp"
#include "archive_test_helpers.hpp"
#include "operation_runs.hpp"

#include <tar_archive/compression.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Test
{
    /**
     * @brief Operations on trees and on the names of entries, on both protocols.
     */
    class ProtocolTreeTests : public RemoteProtocolTests
    {
      protected:
        /**
         * @brief The files of the tree that putTree() puts on the server, by their paths below it.
         */
        static std::map<std::string, std::string> treeFiles()
        {
            return {
                {"a.txt", "alpha"},
                {"sub/b.txt", "beta"},
                {"sub/deeper/c.txt", "gamma"},
            };
        }

        /**
         * @brief Puts the tree "tree" on the server, with an empty directory, and a file beside it.
         */
        void putTree()
        {
            putRemoteDirectory("tree");
            putRemoteDirectory("tree/sub");
            putRemoteDirectory("tree/sub/deeper");
            putRemoteDirectory("tree/empty");
            for (auto const& [path, content] : treeFiles())
                putRemoteFile(fmt::format("tree/{}", path), content);
            putRemoteFile("beside.txt", "beside");
        }

        /**
         * @brief Writes the files of treeFiles() and an empty directory below @p root.
         */
        static void writeLocalTree(std::filesystem::path const& root)
        {
            std::filesystem::create_directories(root / "empty");
            for (auto const& [path, content] : treeFiles())
                writeLocalFile(root / path, content);
        }

        /**
         * @brief The paths of @p entries below the root of their scan, by their types.
         */
        static std::map<std::string, SharedData::FileType>
        pathsOf(std::vector<SharedData::DirectoryEntry> const& entries)
        {
            std::map<std::string, SharedData::FileType> paths{};
            for (auto const& entry : entries)
            {
                const auto path = SharedData::relativePath(entries, entry);
                EXPECT_TRUE(path.has_value()) << "an unusable name '" << entry.name << "'";
                if (path && !path->empty())
                    paths.emplace(path->value(), entry.type);
            }
            return paths;
        }

        static std::map<std::string, SharedData::FileType> expectedTreePaths()
        {
            using enum SharedData::FileType;
            return {
                {"a.txt", Regular},
                {"empty", Directory},
                {"sub", Directory},
                {"sub/b.txt", Regular},
                {"sub/deeper", Directory},
                {"sub/deeper/c.txt", Regular},
            };
        }

        std::unique_ptr<ScanOperation> scanOf(std::string_view relative) const
        {
            return std::make_unique<ScanOperation>(
                fileSystem(), ScanOperation::ScanOperationOptions{.remotePath = remoteAt(relative)}
            );
        }

        void expectTreeBesideUntouched()
        {
            EXPECT_EQ(remoteContentOf("beside.txt"), "beside");
        }
    };

    TEST_P(ProtocolTreeTests, AScanFindsTheWholeTree)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        auto scan = scanOf("tree");

        const auto result = runToEnd(*scan, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        std::uint64_t expectedBytes{0u};
        for (auto const& [path, content] : treeFiles())
            expectedBytes += content.size();
        EXPECT_EQ(scan->totalBytes(), expectedBytes);
        const auto entries = scan->ejectEntries();
        EXPECT_EQ(pathsOf(entries), expectedTreePaths());
        for (auto const& entry : entries)
        {
            const auto path = SharedData::relativePath(entries, entry);
            if (path && treeFiles().contains(path->value()))
                EXPECT_EQ(entry.size, treeFiles().at(path->value()).size()) << path->value();
        }
    }

    TEST_P(ProtocolTreeTests, AScanOfAMissingDirectoryFails)
    {
        auto scan = scanOf("missing");

        const auto result = runToEnd(*scan, operationLimit);

        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().type, Operation::ErrorType::FutureTimeout);
    }

    TEST_P(ProtocolTreeTests, ABulkDownloadCopiesTheTree)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        const auto destination = localDirectory_.path() / "tree";
        BulkDownloadOperation operation{
            fileSystem(), {.remotePath = remoteAt("tree"), .localPath = toLocal(destination)}
        };
        operation.scanFirst(scanOf("tree"));

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        for (auto const& [path, content] : treeFiles())
            EXPECT_EQ(localContentOf(destination / path), content) << path;
        EXPECT_TRUE(std::filesystem::is_directory(destination / "empty"));
        EXPECT_FALSE(std::filesystem::exists(localDirectory_.path() / "beside.txt"));
    }

    TEST_P(ProtocolTreeTests, ABulkDownloadOfSeveralFilesAtOnceCopiesTheTree)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        const auto destination = localDirectory_.path() / "tree";
        BulkDownloadOperation operation{
            fileSystem(),
            {
                .remotePath = remoteAt("tree"),
                .localPath = toLocal(destination),
                .individualOptions = {.bigFileOptimized = true},
                .concurrency = 2,
            }
        };
        operation.scanFirst(scanOf("tree"));

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        for (auto const& [path, content] : treeFiles())
            EXPECT_EQ(localContentOf(destination / path), content) << path;
    }

    TEST_P(ProtocolTreeTests, ABulkDownloadOfAPlanGivesItsFilesTheModificationTimesOfTheirSources)
    {
        // 2023-11-14 22:13:57 UTC. A plan made from a listing to the minute knows 22:13:00.
        constexpr std::uint64_t sourceTime{1700000037u};
        constexpr std::uint64_t plannedTime{sourceTime - sourceTime % 60u};
        const std::map<std::string, std::string> files{{"first.txt", "first"}, {"second.txt", "second content"}};
        ASSERT_NO_FATAL_FAILURE(putRemoteDirectory("planned"));
        for (auto const& [name, content] : files)
        {
            ASSERT_NO_FATAL_FAILURE(putRemoteFile(fmt::format("planned/{}", name), content));
            ASSERT_NO_FATAL_FAILURE(setRemoteModificationTime(fmt::format("planned/{}", name), sourceTime));
        }
        for (const bool inheritPermissions : {true, false})
        {
            SCOPED_TRACE(fmt::format("inheritPermissions: {}", inheritPermissions));
            const auto destination = localDirectory_.path() / fmt::format("planned{}", inheritPermissions);
            BulkDownloadOperation operation{
                fileSystem(),
                {
                    .remotePath = remoteAt("planned"),
                    .localPath = toLocal(destination),
                    .individualOptions = {.inheritPermissions = inheritPermissions, .createMissingDirectories = true},
                }
            };
            std::vector<BulkDownloadOperation::PrescannedFile> planned{};
            for (auto const& [name, content] : files)
                planned.push_back({
                    .source = remoteAt(fmt::format("planned/{}", name)),
                    .destination = toLocal(destination / name),
                    .sizeBytes = content.size(),
                    .mtime = plannedTime,
                    .mtimePrecision = SharedData::TimePrecision::Minute,
                });
            operation.setPrescannedFileList(std::move(planned));

            const auto result = runToEnd(operation, operationLimit);

            ASSERT_TRUE(result.has_value()) << result.error().toString();
            EXPECT_TRUE(operation.getFailed().empty());
            for (auto const& [name, content] : files)
            {
                EXPECT_EQ(localContentOf(destination / name), content) << name;
                EXPECT_EQ(localModificationTimeOf(destination / name), sourceTime) << name;
            }
        }
    }

    TEST_P(ProtocolTreeTests, ABulkUploadCreatesTheTree)
    {
        const auto source = localDirectory_.path() / "tree";
        writeLocalTree(source);
        BulkUploadOperation operation{fileSystem(), {.remotePath = remoteAt("tree"), .localPath = toLocal(source)}};
        operation.scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = toLocal(source)})
        );

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        for (auto const& [path, content] : treeFiles())
        {
            EXPECT_EQ(remoteContentOf(fmt::format("tree/{}", path)), content) << path;
            EXPECT_FALSE(remoteExists(fmt::format("tree/{}.filepart", path))) << path;
        }
        EXPECT_TRUE(remoteHasDirectory("tree/empty"));
    }

    TEST_P(ProtocolTreeTests, ABulkUploadOfSeveralFilesAtOnceCreatesTheTree)
    {
        const auto source = localDirectory_.path() / "tree";
        writeLocalTree(source);
        BulkUploadOperation operation{
            fileSystem(),
            {
                .remotePath = remoteAt("tree"),
                .localPath = toLocal(source),
                .individualOptions = {.bigFileOptimized = true},
                .concurrency = 2,
            }
        };
        operation.scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = toLocal(source)})
        );

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        for (auto const& [path, content] : treeFiles())
            EXPECT_EQ(remoteContentOf(fmt::format("tree/{}", path)), content) << path;
    }

    TEST_P(ProtocolTreeTests, AnArchiveDownloadPacksTheTreeAndAFile)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        const auto archive = localDirectory_.path() / "tree.tar";
        ArchiveDownloadOperation operation{
            fileSystem(),
            {
                .roots =
                    {
                        makeDirectoryEntry(remoteAt("tree").value()),
                        makeFileEntry(remoteAt("beside.txt").value(), 6u),
                    },
                .localArchivePath = toLocal(archive),
                .compression = TarArchive::Compression::None,
            }
        };
        operation.addScanRoot(scanOf("tree"));

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        const auto entries = recoverArchiveEntries(archive);
        for (auto const& [path, content] : treeFiles())
        {
            auto const* entry = findEntry(entries, fmt::format("tree/{}", path));
            ASSERT_NE(entry, nullptr) << path;
            EXPECT_EQ(entry->contents, content) << path;
        }
        auto const* beside = findEntry(entries, "beside.txt");
        ASSERT_NE(beside, nullptr);
        EXPECT_EQ(beside->contents, "beside");
    }

    TEST_P(ProtocolTreeTests, AnArchiveUploadStoresTheArchiveOfTheFiles)
    {
        const auto source = localDirectory_.path() / "tree";
        writeLocalTree(source);
        writeLocalFile(localDirectory_.path() / "beside.txt", "beside");
        ArchiveUploadOperation operation{
            fileSystem(),
            {
                .localPaths = {toLocal(source), toLocal(localDirectory_.path() / "beside.txt")},
                .remoteArchivePath = remoteAt("tree.tar"),
                .compression = TarArchive::Compression::None,
            }
        };
        operation.addScanRoot(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = toLocal(source)})
        );

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(remoteExists("tree.tar.filepart"));
        const auto archive = remoteContentOf("tree.tar");
        ASSERT_TRUE(archive.has_value());
        const auto pulled = localDirectory_.path() / "pulled.tar";
        writeLocalFile(pulled, *archive);
        const auto entries = recoverArchiveEntries(pulled);
        for (auto const& [path, content] : treeFiles())
        {
            auto const* entry = findEntry(entries, fmt::format("tree/{}", path));
            ASSERT_NE(entry, nullptr) << path;
            EXPECT_EQ(entry->contents, content) << path;
        }
        auto const* beside = findEntry(entries, "beside.txt");
        ASSERT_NE(beside, nullptr);
        EXPECT_EQ(beside->contents, "beside");
    }

    TEST_P(ProtocolTreeTests, ADeleteRemovesAFile)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        DeleteOperation operation{fileSystem(), {.remotePath = remoteAt("tree/a.txt")}};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(remoteExists("tree/a.txt"));
        EXPECT_EQ(remoteContentOf("tree/sub/b.txt"), "beta");
        expectTreeBesideUntouched();
    }

    TEST_P(ProtocolTreeTests, ADeleteOfAMissingFileCountsAsDoneAndRemovesNothingElse)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        putRemoteFile("tree/kept.txt", "kept");
        DeleteOperation operation{fileSystem(), {.remotePath = remoteAt("tree/missing.txt")}};

        const auto result = runToEnd(operation, operationLimit);

        // A path that is already gone counts as deleted.
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("tree/kept.txt"), "kept");
        expectTreeBesideUntouched();
    }

    TEST_P(ProtocolTreeTests, ARecursiveDeleteRemovesTheTreeAndNothingBesideIt)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        DeleteOperation operation{fileSystem(), {.remotePath = remoteAt("tree"), .recursive = true}};
        operation.addScanRoot(scanOf("tree"));

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_FALSE(remoteExists("tree"));
        expectTreeBesideUntouched();
        if (auto const* tree = servedTree())
        {
            for (auto const& removed : tree->removed())
                EXPECT_TRUE(removed.starts_with(remoteAt("tree").value())) << removed;
        }
    }

    TEST_P(ProtocolTreeTests, ARenameMovesAFile)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        RenameOperation operation{
            fileSystem(), {.sourcePath = remoteAt("tree/a.txt"), .destinationPath = remoteAt("tree/sub/moved.txt")}
        };

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(remoteExists("tree/a.txt"));
        EXPECT_EQ(remoteContentOf("tree/sub/moved.txt"), "alpha");
    }

    TEST_P(ProtocolTreeTests, ARenameMovesADirectoryWithItsContent)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        RenameOperation operation{
            fileSystem(), {.sourcePath = remoteAt("tree/sub"), .destinationPath = remoteAt("moved")}
        };

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(remoteExists("tree/sub"));
        EXPECT_EQ(remoteContentOf("moved/b.txt"), "beta");
        EXPECT_EQ(remoteContentOf("moved/deeper/c.txt"), "gamma");
    }

    TEST_P(ProtocolTreeTests, ARenameLeavesAnExistingDestination)
    {
        ASSERT_NO_FATAL_FAILURE(putTree());
        // A file onto a file, a directory onto an empty directory: both would replace it on a POSIX server.
        const std::vector<std::pair<std::string_view, std::string_view>> renames{
            {"tree/a.txt", "beside.txt"},
            {"tree/sub/deeper", "tree/empty"},
        };
        for (auto const& [source, destination] : renames)
        {
            SCOPED_TRACE(fmt::format("{} to {}", source, destination));
            RenameOperation operation{
                fileSystem(), {.sourcePath = remoteAt(source), .destinationPath = remoteAt(destination)}
            };

            const auto result = runToEnd(operation, operationLimit);

            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().type, Operation::ErrorType::RenameFailure);
            ASSERT_TRUE(result.error().remoteError.has_value());
            EXPECT_EQ(result.error().remoteError->kind, RemoteIo::RemoteErrorKind::AlreadyExists);
            EXPECT_TRUE(remoteExists(source));
            EXPECT_TRUE(remoteExists(destination));
        }
        EXPECT_EQ(remoteContentOf("tree/a.txt"), "alpha");
        expectTreeBesideUntouched();
        EXPECT_EQ(remoteContentOf("tree/sub/deeper/c.txt"), "gamma");
    }

    INSTANTIATE_FOR_BOTH_PROTOCOLS(ProtocolTreeTests);
}
