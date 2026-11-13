#pragma once

#include <ftp/support/file_system_harness.hpp>
#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/tree_responder.hpp>
#include <remote_io/remote_file_system.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Test
{
    /**
     * @brief A scripted FTP server that serves a tree, and a file system of the backend logged in there.
     */
    class FtpServedTreeTests : public ::testing::Test
    {
      protected:
        using FileSystemHarness = FileTransferProtocol::Test::FileSystemHarness;
        using ScriptedFtpServer = FileTransferProtocol::Test::ScriptedFtpServer;
        using ServedEntry = FileTransferProtocol::Test::ServedEntry;
        using ServedTree = FileTransferProtocol::Test::ServedTree;

        /** @brief How long an operation on the served tree may take. */
        static constexpr std::chrono::seconds operationLimit{30};

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

        static constexpr std::size_t servedConnections{4u};

        std::shared_ptr<ServedTree> tree_{};
        std::unique_ptr<ScriptedFtpServer> server_{};
        std::unique_ptr<FileSystemHarness> harness_{};
    };
}
