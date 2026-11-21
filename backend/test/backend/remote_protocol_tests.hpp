#pragma once

#include <ftp/support/file_system_harness.hpp>
#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/tree_responder.hpp>
#include <remote_io/remote_file_system.hpp>
#include <utility/temporary_directory.hpp>
#include "real_server_tests.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    enum class RemoteProtocol
    {
        Sftp,
        Ftp,
    };

    inline std::string nameOf(RemoteProtocol protocol)
    {
        switch (protocol)
        {
            case RemoteProtocol::Sftp:
                return "Sftp";
            case RemoteProtocol::Ftp:
                return "Ftp";
        }
        return "Unknown";
    }

    inline void PrintTo(RemoteProtocol protocol, std::ostream* stream)
    {
        *stream << nameOf(protocol);
    }

    inline std::string protocolName(::testing::TestParamInfo<RemoteProtocol> const& info)
    {
        return nameOf(info.param);
    }

    /**
     * @brief A server of the protocol the test runs with and a file system logged in there: the SFTP mock, or the
     *        scripted FTP mock serving a tree. Both have the empty directory workDirectory, where the test puts what
     *        it needs, and are looked at from the server's side where it can tell.
     */
    class RemoteProtocolTests
        : public RealServerTests
        , public ::testing::WithParamInterface<RemoteProtocol>
    {
      protected:
        using ServedTree = FileTransferProtocol::Test::ServedTree;

        static constexpr std::string_view workDirectory{"/home/test/work"};
        /** @brief How long an operation may take. */
        static constexpr std::chrono::seconds operationLimit{30};
        /** @brief How long a look at the server may take. */
        static constexpr std::chrono::seconds lookLimit{10};

        void SetUp() override
        {
            RealServerTests::SetUp();
            switch (GetParam())
            {
                case RemoteProtocol::Sftp:
                    startSftp();
                    break;
                case RemoteProtocol::Ftp:
                    startFtp();
                    break;
            }
        }

        void TearDown() override
        {
            stopFtp();
            stopSftp();
        }

        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem() const
        {
            if (harness_)
                return harness_->fileSystem();
            return sftp_;
        }

        /**
         * @brief @p relative below workDirectory.
         */
        static Paths::RemotePath remoteAt(std::string_view relative)
        {
            return remote(fmt::format("{}/{}", workDirectory, relative));
        }

        void putRemoteDirectory(std::string_view relative)
        {
            const auto path = remoteAt(relative).value();
            if (tree_)
            {
                ASSERT_TRUE(tree_->makeDirectory(path)) << path;
                return;
            }
            ASSERT_TRUE(sftpServer_->command("createDirectory", {{"path", path}})) << path;
        }

        void putRemoteFile(std::string_view relative, std::string const& content)
        {
            const auto path = remoteAt(relative).value();
            if (tree_)
            {
                ASSERT_TRUE(tree_->putFile(path, content)) << path;
                return;
            }
            ASSERT_TRUE(sftpServer_->command("createFile", {{"path", path}, {"content", content}})) << path;
        }

        /**
         * @brief Gives the file at @p relative the modification time @p seconds since the epoch.
         */
        void setRemoteModificationTime(std::string_view relative, std::uint64_t seconds)
        {
            const auto path = remoteAt(relative);
            auto setting = fileSystem()->later(
                RemoteIo::SetModificationTime{
                    .path = path,
                    .modificationTime = std::chrono::system_clock::time_point{std::chrono::seconds{seconds}},
                }
            );
            ASSERT_EQ(setting.wait_for(lookLimit), std::future_status::ready) << path.value();
            const auto set = setting.get();
            ASSERT_TRUE(set.has_value()) << path.value() << ": " << set.error().toString();
        }

        /**
         * @brief The modification time of the local file @p path in seconds since the epoch.
         */
        static std::uint64_t localModificationTimeOf(std::filesystem::path const& path)
        {
            const auto since =
                std::chrono::file_clock::to_sys(std::filesystem::last_write_time(path)).time_since_epoch();
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(since).count());
        }

        /**
         * @brief Waits until the processing strand of the file system ran what was queued on it before, like the
         *        removal of the part a stopped transfer left.
         */
        void awaitQueuedWork() const
        {
            auto flushed = fileSystem()->performPromise(
                []()
                {
                    return true;
                }
            );
            if (flushed.wait_for(lookLimit) != std::future_status::ready)
                ADD_FAILURE() << "The processing strand did not run its queue";
        }

        /**
         * @brief The content of the file at @p relative as the server has it once the queued work ran, nothing where
         *        no file is.
         */
        std::optional<std::string> remoteContentOf(std::string_view relative)
        {
            awaitQueuedWork();
            const auto path = remoteAt(relative);
            if (tree_)
                return tree_->contentOf(path.value());
            auto reading = sftp_->later(RemoteIo::ReadFile{.path = path, .maximumSize = 64u * 1024u * 1024u});
            if (reading.wait_for(lookLimit) != std::future_status::ready)
            {
                ADD_FAILURE() << "No answer to the read of " << path.value();
                return std::nullopt;
            }
            auto content = reading.get();
            if (!content)
                return std::nullopt;
            return std::move(content).value();
        }

        /**
         * @brief Whether the server has an entry at @p relative once the queued work ran, a link not followed.
         */
        bool remoteExists(std::string_view relative)
        {
            awaitQueuedWork();
            const auto path = remoteAt(relative);
            if (tree_)
                return tree_->exists(path.value());
            auto looking = sftp_->later(RemoteIo::Lstat{.path = path});
            if (looking.wait_for(lookLimit) != std::future_status::ready)
            {
                ADD_FAILURE() << "No answer to the lookup of " << path.value();
                return false;
            }
            const auto found = looking.get();
            if (!found && !RemoteIo::isMissing(found.error()))
                ADD_FAILURE() << "The lookup of " << path.value() << " failed: " << found.error().toString();
            return found.has_value();
        }

        /**
         * @brief Whether the server has a directory at @p relative once the queued work ran.
         */
        bool remoteHasDirectory(std::string_view relative)
        {
            awaitQueuedWork();
            const auto path = remoteAt(relative);
            if (tree_)
                return tree_->directoryAt(path.value()).has_value();
            auto looking = sftp_->later(RemoteIo::Lstat{.path = path});
            if (looking.wait_for(lookLimit) != std::future_status::ready)
            {
                ADD_FAILURE() << "No answer to the lookup of " << path.value();
                return false;
            }
            const auto found = looking.get();
            return found && found->type == SharedData::FileType::Directory;
        }

        static void writeLocalFile(std::filesystem::path const& path, std::string_view content)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file{path, std::ios::binary};
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
        }

        static std::optional<std::string> localContentOf(std::filesystem::path const& path)
        {
            std::ifstream file{path, std::ios::binary};
            if (!file)
                return std::nullopt;
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        /**
         * @brief @p size bytes that differ along the way, so a part continued at the wrong place shows.
         */
        static std::string patternedContent(std::size_t size)
        {
            std::string content(size, '\0');
            for (std::size_t index = 0u; index < size; ++index)
                content[index] = static_cast<char>('a' + index % 23u);
            return content;
        }

        /**
         * @brief Lets FTP downloads that start from now on stall after @p bytes, nothing lets them run whole again.
         *        The SFTP mock sends a big file slowly enough by itself to stop its download under way.
         */
        void stallFtpDownloadsAfter(std::optional<std::uint64_t> bytes)
        {
            if (tree_)
                tree_->stallDownloadsAfter(bytes);
        }

        /**
         * @brief The tree the FTP server serves, null for SFTP.
         */
        ServedTree* servedTree() const
        {
            return tree_.get();
        }

        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};

      private:
        /** @brief C0, the transfer connections and spares for connections the pool replaces. */
        static constexpr std::size_t servedConnections{8u};
        static constexpr std::size_t transferConnections{2u};

        void startSftp()
        {
            auto [server, thread] = createServer(sftpServer);
            ASSERT_TRUE(server);
            sftpServer_ = std::move(server);
            sftpThread_ = std::move(thread);
            auto [session, sftp] = createSftpSession(sftpServer_->port);
            sshSession_ = std::move(session);
            sftp_ = std::move(sftp);
            ASSERT_TRUE(sftpServer_->command("createDirectory", {{"path", std::string{workDirectory}}}));
        }

        void stopSftp()
        {
            sftp_.reset();
            sshSession_.reset();
            if (sftpServer_)
                sftpServer_->command("exit");
            if (sftpThread_.joinable())
                sftpThread_.join();
            sftpServer_.reset();
        }

        void startFtp()
        {
            using FileTransferProtocol::Test::servedDirectory;
            tree_ = std::make_shared<ServedTree>(
                std::vector{servedDirectory("home", {servedDirectory("test", {servedDirectory("work", {})})})}
            );
            auto started = FileTransferProtocol::Test::ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            ftpServer_ = std::move(started).value();
            for (std::size_t connection = 0u; connection < servedConnections; ++connection)
                ftpServer_->addConnection(FileTransferProtocol::Test::serving(tree_));
            auto connected = FileTransferProtocol::Test::FileSystemHarness::connect({
                .host = ftpServer_->address(),
                .port = ftpServer_->port(),
                .transferConnections = transferConnections,
            });
            ASSERT_TRUE(connected) << connected.error();
            harness_ = std::move(connected).value();
        }

        void stopFtp()
        {
            harness_.reset();
            if (!ftpServer_)
                return;
            EXPECT_TRUE(ftpServer_->waitUntilDisconnected());
            for (auto const& failure : ftpServer_->failures())
                ADD_FAILURE() << failure;
            if (HasFailure())
            {
                for (std::size_t connection = 0u; connection < servedConnections; ++connection)
                {
                    std::string commands{};
                    for (auto const& command : ftpServer_->commands(connection))
                        commands += fmt::format("\n  {}", command);
                    std::cout << fmt::format("Commands of connection {}:{}\n", connection, commands);
                }
            }
            ftpServer_.reset();
        }

        std::shared_ptr<SecureShell::Test::NodeProcessResult> sftpServer_{};
        std::thread sftpThread_{};
        std::unique_ptr<SecureShell::Session> sshSession_{};
        std::shared_ptr<SecureShell::SftpSession> sftp_{};

        std::shared_ptr<ServedTree> tree_{};
        std::unique_ptr<FileTransferProtocol::Test::ScriptedFtpServer> ftpServer_{};
        std::unique_ptr<FileTransferProtocol::Test::FileSystemHarness> harness_{};
    };

#define INSTANTIATE_FOR_BOTH_PROTOCOLS(suite) \
    INSTANTIATE_TEST_SUITE_P( \
        Protocols, suite, ::testing::Values(RemoteProtocol::Sftp, RemoteProtocol::Ftp), protocolName \
    )
}
