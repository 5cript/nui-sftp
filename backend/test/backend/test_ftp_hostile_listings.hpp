#pragma once

#include <backend/sftp/bulk_download_operation.hpp>
#include <backend/sftp/delete_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <ftp/support/tree_responder.hpp>
#include <utility/temporary_directory.hpp>
#include "ftp_served_tree_tests.hpp"
#include "operation_runs.hpp"
#include "real_server_tests.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace
    {
        using FileTransferProtocol::Test::ServedEntry;
        using FileTransferProtocol::Test::servedDirectory;
        using FileTransferProtocol::Test::servedFile;
        using FileTransferProtocol::Test::servedLink;

        /**
         * @brief A server that lies in its listing of /tree: names that are no child, that climb out of a directory
         *        on some system, that repeat, that a line end cuts, and links out of the tree and onto itself. Beside
         *        the tree lie what nothing may touch.
         */
        std::vector<ServedEntry> hostileRoot()
        {
            return {
                servedFile("outside.txt", "outside"),
                servedDirectory("other", {servedFile("keep.txt", "keep")}),
                servedDirectory(
                    "tree",
                    {
                        servedFile("a.txt", "alpha"),
                        servedFile("a.txt", "a second alpha"),
                        servedDirectory("sub", {servedFile("b.txt", "beta")}),
                        servedFile(".", "dot"),
                        servedFile("..", "dot dot"),
                        servedFile("", "empty"),
                        servedFile("x/y", "slash"),
                        servedFile("../../escape", "escape"),
                        servedFile("..\\..\\escape", "backslash escape"),
                        servedFile("C:", "drive"),
                        servedFile("C:\\evil", "drive path"),
                        servedFile(" spaced ", "spaces"),
                        servedFile(std::string{"nul\0byte", 8u}, "nul"),
                        servedFile("cr\rname", "cr"),
                        servedFile("lf\nname", "lf"),
                        servedFile(std::string(2000u, 'n'), "long"),
                        servedLink("root", "/"),
                        servedLink("up", ".."),
                        servedLink("self", "."),
                        servedLink("home", "/tree"),
                        servedLink("sneaky", "../outside.txt"),
                        servedLink("dangling", "/nowhere"),
                    }
                ),
            };
        }

        std::string contentOf(std::filesystem::path const& path)
        {
            std::ifstream file{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        bool liesInTheTree(std::string const& path)
        {
            return path == "/tree" || path.starts_with("/tree/");
        }
    }

    class FtpHostileListingTests : public FtpServedTreeTests
    {
      protected:
        std::vector<SharedData::DirectoryEntry> scanTree(std::uint64_t& totalBytes)
        {
            ScanOperation scan{fileSystem(), {.remotePath = remote("/tree")}};
            const auto scanned = runToEnd(scan, operationLimit);
            EXPECT_TRUE(scanned.has_value()) << scanned.error().toString();
            totalBytes = scan.totalBytes();
            return scan.ejectEntries();
        }

        void expectListedOnlyTheTree() const
        {
            const auto listed = tree_->listed();
            EXPECT_FALSE(listed.empty());
            for (auto const& directory : listed)
                EXPECT_TRUE(liesInTheTree(directory)) << "listed " << directory;
        }
    };

    TEST_F(FtpHostileListingTests, AScanKeepsToTheTreeAndEnds)
    {
        serve(hostileRoot());
        std::uint64_t totalBytes{0u};
        const auto entries = scanTree(totalBytes);

        std::vector<std::string> names{};
        for (auto const& entry : entries)
        {
            if (!entry.parent)
                continue;
            EXPECT_TRUE(entry.nameSegment().has_value()) << "scanned the name '" << entry.name << "'";
            names.push_back(entry.name);
        }
        EXPECT_NE(std::ranges::find(names, "a.txt"), names.end());
        EXPECT_NE(std::ranges::find(names, "b.txt"), names.end());
        EXPECT_EQ(std::ranges::count(names, "a.txt"), 1);
        expectListedOnlyTheTree();
    }

    TEST_F(FtpHostileListingTests, ABulkDownloadWritesOnlyBelowItsDestination)
    {
        serve(hostileRoot());
        std::uint64_t totalBytes{0u};
        auto entries = scanTree(totalBytes);
        ASSERT_FALSE(entries.empty());

        const Utility::TemporaryDirectory run{temporaryRoot, true};
        const auto destination = run.path() / "tree";
        BulkDownloadOperation operation{
            fileSystem(), {.remotePath = remote("/tree"), .localPath = toLocal(destination)}
        };
        operation.setScanResult(std::move(entries), totalBytes);
        const auto result = runToEnd(operation, operationLimit);
        ASSERT_TRUE(result.has_value()) << result.error().toString();

        std::vector<std::filesystem::path> besideIt{};
        for (auto const& item : std::filesystem::directory_iterator{run.path()})
            besideIt.push_back(item.path().filename());
        EXPECT_EQ(besideIt, (std::vector<std::filesystem::path>{"tree"}));
        EXPECT_EQ(contentOf(destination / "a.txt"), "alpha");
        EXPECT_EQ(contentOf(destination / "sub" / "b.txt"), "beta");
        // Links stay links, none was followed into a directory.
        for (auto const& item : std::filesystem::recursive_directory_iterator{destination})
        {
            if (std::filesystem::is_directory(item.symlink_status()))
                EXPECT_EQ(item.path(), destination / "sub") << "created " << item.path();
        }
        const auto retrieved = tree_->retrieved();
        EXPECT_FALSE(retrieved.empty());
        for (auto const& path : retrieved)
            EXPECT_TRUE(liesInTheTree(path)) << "retrieved " << path;
    }

    TEST_F(FtpHostileListingTests, ARecursiveDeleteRemovesOnlyBelowItsTarget)
    {
        serve(hostileRoot());
        DeleteOperation operation{fileSystem(), {.remotePath = remote("/tree"), .recursive = true}};
        operation.addScanRoot(
            std::make_unique<ScanOperation>(fileSystem(), ScanOperation::ScanOperationOptions{.remotePath = remote("/tree")})
        );
        const auto result = runToEnd(operation, operationLimit);
        ASSERT_TRUE(result.has_value() || result.error().type != Operation::ErrorType::FutureTimeout)
            << "the delete did not end";

        const auto removed = tree_->removed();
        EXPECT_FALSE(removed.empty());
        for (auto const& path : removed)
            EXPECT_TRUE(liesInTheTree(path)) << "removed " << path;
        EXPECT_TRUE(tree_->exists("/outside.txt"));
        EXPECT_TRUE(tree_->exists("/other/keep.txt"));
        EXPECT_FALSE(tree_->exists("/tree/sub/b.txt"));
    }

    TEST_F(FtpHostileListingTests, ANameListedAsAFileAndAsADirectoryEndsTheScan)
    {
        // The directory counts for the listing, the file for the server's lookups: entering it fails.
        serve({servedDirectory(
            "tree", {servedFile("twice", "file"), servedDirectory("twice", {servedFile("inner.txt", "inner")})}
        )});
        ScanOperation scan{fileSystem(), {.remotePath = remote("/tree")}};
        const auto scanned = runToEnd(scan, operationLimit);
        ASSERT_TRUE(scanned.has_value() || scanned.error().type != Operation::ErrorType::FutureTimeout)
            << "the scan did not end";
        expectListedOnlyTheTree();
    }
}
