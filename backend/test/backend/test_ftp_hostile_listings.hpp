#pragma once

#include <backend/sftp/bulk_download_operation.hpp>
#include <backend/sftp/delete_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <ftp/support/file_system_harness.hpp>
#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/tree_responder.hpp>
#include <utility/temporary_directory.hpp>
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
#include <thread>
#include <utility>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace
    {
        using FileTransferProtocol::Test::FileSystemHarness;
        using FileTransferProtocol::Test::ScriptedFtpServer;
        using FileTransferProtocol::Test::ServedEntry;
        using FileTransferProtocol::Test::ServedTree;
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

        /**
         * @brief Runs @p operation until it completes or fails, at most 30 s.
         */
        template <typename OperationT>
        std::expected<Operation::WorkStatus, Operation::Error> runToEnd(OperationT& operation)
        {
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto result = operation.work();
                if (!result.has_value() || result.value() == Operation::WorkStatus::Complete)
                    return result;
                // Waiting work returns at once, polling it is how the queue drives it too.
                std::this_thread::sleep_for(2ms);
            }
            return std::unexpected(Operation::Error{.type = Operation::ErrorType::FutureTimeout});
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

    class FtpHostileListingTests : public ::testing::Test
    {
      protected:
        /**
         * @brief Serves @p root and opens a file system there.
         */
        void serve(std::vector<ServedEntry> root)
        {
            tree_ = std::make_shared<ServedTree>(std::move(root));
            auto started = ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
            // C0, the transfer connection and spares for connections the pool replaces.
            for (std::size_t connection = 0u; connection < servedConnections; ++connection)
                server_->addConnection(FileTransferProtocol::Test::serving(tree_));
            auto connected = FileSystemHarness::connect(
                FileSystemHarness::Options{.host = server_->address(), .port = server_->port()}
            );
            ASSERT_TRUE(connected) << connected.error();
            harness_ = std::move(connected).value();
        }

        void TearDown() override
        {
            harness_.reset();
            if (!server_)
                return;
            EXPECT_TRUE(server_->waitUntilDisconnected());
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
            if (!HasFailure())
                return;
            for (std::size_t connection = 0u; connection < servedConnections; ++connection)
            {
                std::string commands{};
                for (auto const& command : server_->commands(connection))
                    commands += fmt::format("\n  {}", command);
                std::cout << fmt::format("Commands of connection {}:{}\n", connection, commands);
            }
        }

        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem() const
        {
            return harness_->fileSystem();
        }

        std::vector<SharedData::DirectoryEntry> scanTree(std::uint64_t& totalBytes)
        {
            ScanOperation scan{fileSystem(), {.remotePath = remote("/tree")}};
            const auto scanned = runToEnd(scan);
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

        static constexpr std::size_t servedConnections{4u};

        std::shared_ptr<ServedTree> tree_{};
        std::unique_ptr<ScriptedFtpServer> server_{};
        std::unique_ptr<FileSystemHarness> harness_{};
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
        const auto result = runToEnd(operation);
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
        const auto result = runToEnd(operation);
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
        const auto scanned = runToEnd(scan);
        ASSERT_TRUE(scanned.has_value() || scanned.error().type != Operation::ErrorType::FutureTimeout)
            << "the scan did not end";
        expectListedOnlyTheTree();
    }
}
