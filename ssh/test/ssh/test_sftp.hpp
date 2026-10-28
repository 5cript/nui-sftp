#pragma once

#include "common_fixture.hpp"
#include <ssh/session.hpp>
#include <nui/utility/scope_exit.hpp>
#include <ssh/sftp_session.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/async/transfer_context.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <expected>
#include <future>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace SecureShell::Test
{
    /** @brief How long a test waits for a single answer of the local server, generous for stalled CI runners. */
    constexpr std::chrono::seconds replyTimeout{5};

    class SftpTests : public CommonFixture
    {
      public:
        void TearDown() override
        {
            if (std::filesystem::exists(isolateDirectory_.path() / "log.txt"))
            {
                if (!std::filesystem::exists(programDirectory / "logs"))
                    std::filesystem::create_directory(programDirectory / "logs");

                std::filesystem::rename(
                    isolateDirectory_.path() / "log.txt",
                    programDirectory / "logs" /
                        ("log_"s + ::testing::UnitTest::GetInstance()->current_test_info()->test_case_name() + "_"s +
                         ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".txt"));
            }
        }

        std::pair<std::unique_ptr<Session>, std::shared_ptr<SftpSession>> createSftpSession(unsigned short port)
        {
            auto session = makePasswordTestSession(port);
            if (!session.has_value())
                throw std::runtime_error("Failed to create session");

            (*session)->start();
            auto sftpFuture = (*session)->createSftpSession();
            if (sftpFuture.wait_for(10s) != std::future_status::ready)
                throw std::runtime_error("Failed to create sftp session");

            return std::make_pair(std::move(session).value(), sftpFuture.get().value().lock());
        }

        std::string createAlphabetString(std::size_t length)
        {
            std::string result;
            result.reserve(length);
            for (std::size_t i = 0; i < length; ++i)
            {
                result.push_back('a' + (i % 26));
            }
            return result;
        }

      protected:
    };

    TEST_F(SftpTests, CanCreateSftpSession)
    {
        CREATE_SERVER_AND_JOINER(Sftp);

        auto client = makePasswordTestSession(serverStartResult->port);
        ASSERT_TRUE(client.has_value());
        (*client)->start();
        auto sftpFuture = (*client)->createSftpSession();
        ASSERT_TRUE(sftpFuture.wait_for(10s) == std::future_status::ready);
        auto sftp = sftpFuture.get();
        EXPECT_TRUE(sftp.has_value());
    }

    TEST_F(SftpTests, SftpSessionAndSessionCloseAfterATaskOnTheirThreadThrew)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        auto opened = sftp->openFile(
            remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(opened.wait_for(replyTimeout), std::future_status::ready);
        ASSERT_TRUE(opened.get().has_value());

        sftp->perform(
            []
            {
                throw std::runtime_error("boom");
            }
        );
        // Once the loop ended, promise tasks complete with an error at once.
        bool refused = false;
        const auto deadline = std::chrono::steady_clock::now() + replyTimeout;
        while (!refused && std::chrono::steady_clock::now() < deadline)
        {
            auto probe = sftp->performPromise(
                []() -> std::expected<void, SftpError>
                {
                    return {};
                }
            );
            refused = probe.wait_for(replyTimeout) == std::future_status::ready && !probe.get().has_value();
        }
        ASSERT_TRUE(refused);
        auto another = session->createSftpSession();
        ASSERT_EQ(another.wait_for(0s), std::future_status::ready);
        EXPECT_FALSE(another.get().has_value());

        auto closing = std::async(
            std::launch::async,
            [&session, &sftp]
            {
                const auto closed = sftp->close();
                session.reset();
                return closed;
            }
        );
        ASSERT_EQ(closing.wait_for(10s), std::future_status::ready);
        EXPECT_TRUE(closing.get());
    }

    TEST_F(SftpTests, CanListDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_GT(result.value().size(), 0);
        auto it = std::find_if(result.value().begin(), result.value().end(), [](const auto& entry) {
            return entry.name == "file1.txt";
        });
        EXPECT_NE(it, result.value().end());
    }

    TEST_F(SftpTests, ListingNonExistantDirectoryMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->listDirectory(remote("/home/test/nonexistant"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanCreateDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createDirectory(remote("/home/test/newdir"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.name == "newdir";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CreatingDirectoryThatAlreadyExistsMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createDirectory(remote("/home/test"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanCreateFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile(remote("/home/test/newfile.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.name == "newfile.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CreatingAnExistingFileFailsAndKeepsIt)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile(remote("/home/test/file1.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        EXPECT_FALSE(fut.get().has_value());

        auto statFut = sftp->stat(remote("/home/test/file1.txt"));
        ASSERT_EQ(statFut.wait_for(replyTimeout), std::future_status::ready);
        const auto information = statFut.get();
        ASSERT_TRUE(information.has_value());
        EXPECT_EQ(information->size, std::string{"Fake file content"}.size());
    }

    TEST_F(SftpTests, CreatingFileInNonExistantDirectoryMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile(remote("/home/test/nonexistant/newfile.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanRemoveFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->removeFile(remote("/home/test/file1.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.name == "file1.txt";
        });
        EXPECT_EQ(it, listResult.value().end());
    }

    TEST_F(SftpTests, RemovingNonExistantFileMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->removeFile(remote("/home/test/nonexistant.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanStatFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->stat(remote("/home/test/file1.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        EXPECT_GT(result.value().size, 0);
    }

    TEST_F(SftpTests, HomeDirectoryIsLoginDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/home/test");
    }

    TEST_F(SftpTests, HomeDirectoryIsNotAssumedToBeInHome)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        serverStartResult->command("macHome");
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/Users/test");

        auto listFut = sftp->listDirectory(*result);
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        const auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value()) << listResult.error().message;
        EXPECT_FALSE(listResult->empty());
    }

    TEST_F(SftpTests, HomeDirectoryFallsBackWhenServerAnswersRelativePath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        serverStartResult->command("macHome");
        serverStartResult->command("realpathDotRelative");
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/home/test");
    }

    TEST_F(SftpTests, HomeDirectoryFallsBackWhenServerAnswersEmptyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        serverStartResult->command("macHome");
        serverStartResult->command("realpathDotEmpty");
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/home/test");
    }

    TEST_F(SftpTests, HomeDirectoryFallsBackWhenServerFailsToCanonicalize)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        serverStartResult->command("macHome");
        serverStartResult->command("realpathDotFailure");
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/home/test");

        auto listFut = sftp->listDirectory(*result);
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        const auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value()) << listResult.error().message;
        EXPECT_FALSE(listResult->empty());
    }

    TEST_F(SftpTests, HomeDirectoryFallsBackToRootWhenGuessedHomeIsMissing)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        serverStartResult->command("realpathDotFailure");
        serverStartResult->command("removeHome");
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/");
    }

    TEST_F(SftpTests, CanReadUserDirectories)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->userDirectories(remote("/home/deutsch"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().toString();

        std::vector<std::pair<std::string, std::string>> directories;
        for (auto const& directory : *result)
            directories.emplace_back(directory.name, directory.path);
        EXPECT_EQ(
            directories,
            (std::vector<std::pair<std::string, std::string>>{
                {"DESKTOP", "/home/deutsch/Schreibtisch"},
                {"DOWNLOAD", "/home/deutsch/Downloads"},
                {"DOCUMENTS", "/home/deutsch/Dokumente"},
                {"MUSIC", "/srv/musik"},
                {"PICTURES", "/home/deutsch/Bilder"},
                {"VIDEOS", "/home/deutsch/Videos"},
            })
        );
    }

    TEST_F(SftpTests, MissingUserDirectoriesFileYieldsNoDirectories)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->userDirectories(remote("/home/test"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(result->empty());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        EXPECT_TRUE(listFut.get().has_value());
    }

    TEST_F(SftpTests, CanRenameFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename(remote("/home/test/file1.txt"), remote("/home/test/file1_renamed.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.name == "file1_renamed.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CanRenameDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename(remote("/home/test/Documents"), remote("/home/test/Documents_renamed"));
        ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.name == "Documents_renamed";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CanReadFilePartly)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        char byte;
        auto readFut = file->readSome(&byte, 1);
        ASSERT_EQ(readFut.wait_for(replyTimeout), std::future_status::ready);
        auto readResult = readFut.get();
        ASSERT_TRUE(readResult.has_value());

        EXPECT_EQ(static_cast<char>(byte), 'F');
    }

    TEST_F(SftpTests, CanReadFileFully)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::vector<char> buffer(1024);
        auto readFut = file->readSome(buffer.data(), buffer.size());

        ASSERT_EQ(readFut.wait_for(replyTimeout), std::future_status::ready);
        auto readResult = readFut.get();
        ASSERT_TRUE(readResult.has_value());

        const std::string data = std::string{reinterpret_cast<char*>(buffer.data()), readResult.value()};
        EXPECT_EQ(data, "Fake file content");
    }

    TEST_F(SftpTests, CanReadFileFullyChunkMethod)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::string data;
        auto readFut = file->readAll([&data](std::string_view chunk) {
            data.append(chunk);
            return true;
        });

        ASSERT_EQ(readFut.wait_for(5s), std::future_status::ready);
        auto readResult = readFut.get();
        ASSERT_TRUE(readResult.has_value());

        EXPECT_EQ(data, "Fake file content");
    }

    TEST_F(SftpTests, ReadAllFinishesItsCallbacksBeforeTheResult)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = result->lock();
        ASSERT_TRUE(file);

        // Callers capture locals by reference and leave once the result is there.
        auto finalCallbackDone = std::make_shared<std::atomic_bool>(false);
        auto readFut = file->readAll(
            [finalCallbackDone](std::string_view chunk)
            {
                if (chunk.empty())
                {
                    std::this_thread::sleep_for(100ms);
                    *finalCallbackDone = true;
                }
                return true;
            }
        );
        ASSERT_EQ(readFut.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(readFut.get().has_value());
        EXPECT_TRUE(*finalCallbackDone);
    }

    TEST_F(SftpTests, CanReadBigFileChunkMethod)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::string data;
        auto readFut = file->readAll([&data](std::string_view chunk) {
            data.append(chunk);
            return true;
        });

        ASSERT_EQ(readFut.wait_for(10s), std::future_status::ready);
        auto readResult = readFut.get();
        ASSERT_TRUE(readResult.has_value());

        EXPECT_EQ(data, createAlphabetString(65536));
    }

    TEST_F(SftpTests, CanMoveReadCursorForward)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        ASSERT_EQ(file->seek(2).wait_for(replyTimeout), std::future_status::ready);
        std::vector<char> buffer(1024);
        auto readFut2 = file->readSome(buffer.data(), buffer.size());

        ASSERT_EQ(readFut2.wait_for(replyTimeout), std::future_status::ready);
        auto readResult2 = readFut2.get();
        ASSERT_TRUE(readResult2.has_value());

        const std::string data2 = std::string{reinterpret_cast<char*>(buffer.data()), readResult2.value()};
        EXPECT_EQ(data2, /*Fa*/ "ke file content");
    }

    TEST_F(SftpTests, CanRewindFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::vector<char> buffer(1024);
        ASSERT_EQ(file->seek(2).wait_for(replyTimeout), std::future_status::ready);
        ASSERT_EQ(file->rewind().wait_for(replyTimeout), std::future_status::ready);
        auto readFut2 = file->readSome(buffer.data(), buffer.size());

        ASSERT_EQ(readFut2.wait_for(replyTimeout), std::future_status::ready);
        auto readResult2 = readFut2.get();
        ASSERT_TRUE(readResult2.has_value());

        const std::string data2 = std::string{reinterpret_cast<char*>(buffer.data()), readResult2.value()};
        EXPECT_EQ(data2, "Fake file content");
    }

    TEST_F(SftpTests, CanTellPositionAfterMovingIt)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        ASSERT_EQ(file->seek(2).wait_for(replyTimeout), std::future_status::ready);
        auto tellFut = file->tell();

        ASSERT_EQ(tellFut.wait_for(replyTimeout), std::future_status::ready);
        auto tellResult = tellFut.get();
        ASSERT_TRUE(tellResult.has_value());

        EXPECT_EQ(tellResult.value(), 2);
    }

    TEST_F(SftpTests, CanLetSftpSessionMoveOutOfScope)
    {
        CREATE_SERVER_AND_JOINER(Sftp);

        {
            auto [_, sftp] = createSftpSession(serverStartResult->port);
        }
    }

    TEST_F(SftpTests, CanLetSftpSessionMoveOutOfScopeWithOpenFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);

        {
            auto [_, sftp] = createSftpSession(serverStartResult->port);
            auto fut =
                sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
            ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
            auto result = fut.get();
            ASSERT_TRUE(result.has_value());
        }
    }

    TEST_F(SftpTests, CannotCloseSftpSessionTwice)
    {
        CREATE_SERVER_AND_JOINER(Sftp);

        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_TRUE(sftp->close());
        EXPECT_FALSE(sftp->close());
    }

    TEST_F(SftpTests, CanStatFileUsingFileStream)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        auto statFut = file->stat();
        ASSERT_EQ(statFut.wait_for(replyTimeout), std::future_status::ready);
        auto statResult = statFut.get();
        ASSERT_TRUE(statResult.has_value());

        EXPECT_GT(statResult.value().size, 0);
    }

    TEST_F(SftpTests, CanReadFileAsync)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = std::move(result).value().lock();
        ASSERT_TRUE(file);

        constexpr std::size_t totalSize = 65536;
        using SignedSize = SecureShell::IFileStream::SignedSizeType;

        std::array<char, 4096> buffer{};
        std::string data;
        data.reserve(totalSize);

        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&data, &buffer](SignedSize bytesRead)
            {
                data.append(buffer.data(), static_cast<std::size_t>(bytesRead));
                return true;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(replyTimeout), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!ctx->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        EXPECT_TRUE(ctx->hasEnded());
        EXPECT_EQ(ctx->bytesTransferred(), static_cast<SignedSize>(totalSize));
        EXPECT_EQ(data, createAlphabetString(totalSize));
    }

    TEST_F(SftpTests, AsyncReadOfAServerAnsweringShortAsksOnlyForWhatIsMissing)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        constexpr std::size_t totalSize = 65536;
        ASSERT_TRUE(serverStartResult->command("capReads", {{"bytes", 700}}));
        // A byte asked for twice is served beyond the file's size, which fails the read.
        ASSERT_TRUE(serverStartResult->command("failReadsAfter", {{"bytes", totalSize}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);

        using SignedSize = SecureShell::IFileStream::SignedSizeType;
        std::array<char, 1024> buffer{};
        std::string data;
        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&data, &buffer](SignedSize bytesRead)
            {
                data.append(buffer.data(), static_cast<std::size_t>(bytesRead));
                return true;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(replyTimeout), std::future_status::ready);
        auto ctx = ctxFut.get().value();

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!ctx->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        EXPECT_EQ(ctx->outcome(), AsyncTransferContext::Outcome::Completed);
        EXPECT_EQ(data, createAlphabetString(totalSize));
    }

    namespace
    {
        /**
         * @brief Waits until the transfer counted every piece the callback saw, it adds a piece after the callback.
         */
        bool waitForCountedPieces(
            AsyncTransferContext const& context,
            std::atomic<IFileStream::SignedSizeType> const& seenByCallback
        )
        {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (context.bytesTransferred() != seenByCallback.load())
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                std::this_thread::sleep_for(5ms);
            }
            return true;
        }
    }

    TEST_F(SftpTests, AsyncReadCanBePaused)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = std::move(result).value().lock();
        ASSERT_TRUE(file);

        constexpr std::size_t totalSize = 65536;
        using SignedSize = SecureShell::IFileStream::SignedSizeType;

        std::array<char, 1024> buffer{};
        std::atomic<SignedSize> readCount{0};
        // The first piece waits for the test, the whole file would arrive before it could act otherwise.
        // The test waits for that piece too, a piece still in flight when it acts would arrive after it looked.
        std::promise<void> firstPieceArrived{};
        auto firstPiece = firstPieceArrived.get_future();
        std::promise<void> testActed{};
        const auto acted = testActed.get_future().share();

        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&readCount, &firstPieceArrived, acted](SignedSize bytesRead)
            {
                if (readCount.fetch_add(bytesRead) == 0)
                {
                    firstPieceArrived.set_value();
                    acted.wait_for(5s);
                }
                return true;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(replyTimeout), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();
        ASSERT_EQ(firstPiece.wait_for(replyTimeout), std::future_status::ready);

        ctx->pause(true);
        testActed.set_value();
        ASSERT_TRUE(waitForCountedPieces(*ctx, readCount));
        const auto snapshot = ctx->bytesTransferred();
        std::this_thread::sleep_for(200ms);
        EXPECT_EQ(ctx->bytesTransferred(), snapshot);
        EXPECT_EQ(readCount.load(), snapshot);
        EXPECT_FALSE(ctx->hasEnded());

        ctx->pause(false);

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!ctx->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        EXPECT_TRUE(ctx->hasEnded());
        EXPECT_EQ(ctx->bytesTransferred(), static_cast<SignedSize>(totalSize));
    }

    TEST_F(SftpTests, AsyncReadCanBeCancelled)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = std::move(result).value().lock();
        ASSERT_TRUE(file);

        constexpr std::size_t totalSize = 65536;
        using SignedSize = SecureShell::IFileStream::SignedSizeType;

        std::array<char, 1024> buffer{};
        std::atomic<SignedSize> readCount{0};
        // The first piece waits for the test, the whole file would arrive before it could act otherwise.
        // The test waits for that piece too, a piece still in flight when it acts would arrive after it looked.
        std::promise<void> firstPieceArrived{};
        auto firstPiece = firstPieceArrived.get_future();
        std::promise<void> testActed{};
        const auto acted = testActed.get_future().share();

        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&readCount, &firstPieceArrived, acted](SignedSize bytesRead)
            {
                if (readCount.fetch_add(bytesRead) == 0)
                {
                    firstPieceArrived.set_value();
                    acted.wait_for(5s);
                }
                return true;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(replyTimeout), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();
        ASSERT_EQ(firstPiece.wait_for(replyTimeout), std::future_status::ready);

        ctx->pause(true);
        ctx->cancel();
        testActed.set_value();

        EXPECT_TRUE(ctx->hasEnded());

        ASSERT_TRUE(waitForCountedPieces(*ctx, readCount));
        const auto afterCancel = ctx->bytesTransferred();
        std::this_thread::sleep_for(200ms);
        EXPECT_EQ(ctx->bytesTransferred(), afterCancel);
        EXPECT_LT(ctx->bytesTransferred(), static_cast<SignedSize>(totalSize));
    }

    TEST_F(SftpTests, AsyncReadStopsWhenCallbackReturnsFalse)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = std::move(result).value().lock();
        ASSERT_TRUE(file);

        constexpr std::size_t totalSize = 65536;
        using SignedSize = SecureShell::IFileStream::SignedSizeType;

        std::array<char, 1024> buffer{};
        std::atomic<int> callCount{0};

        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&callCount](SignedSize)
            {
                return ++callCount < 3;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(replyTimeout), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();

        const auto deadline = std::chrono::steady_clock::now() + replyTimeout;
        while (!ctx->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        EXPECT_TRUE(ctx->hasEnded());
        EXPECT_EQ(callCount.load(), 3);
        EXPECT_LT(ctx->bytesTransferred(), static_cast<SignedSize>(totalSize));
    }

    namespace
    {
        /**
         * @brief An asynchronous read of large.txt in pieces of 1 KiB whose first piece waits until the test
         *        has done what it wants to do while requests are in flight.
         */
        struct GatedRead
        {
            static constexpr IFileStream::SignedSizeType fileSize = 65536;

            std::array<char, 1024> buffer{};
            std::atomic<IFileStream::SignedSizeType> received{0};
            std::promise<void> release{};
            std::shared_future<void> released{release.get_future().share()};
            std::shared_ptr<FileStream> file{};
            std::shared_ptr<AsyncTransferContext> context{};

            ::testing::AssertionResult start(SftpSession& sftp, IFileStream::SignedSizeType expectedSize = fileSize)
            {
                auto opened =
                    sftp.openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
                if (opened.wait_for(replyTimeout) != std::future_status::ready)
                    return ::testing::AssertionFailure() << "open timed out";
                auto result = opened.get();
                if (!result.has_value())
                    return ::testing::AssertionFailure() << "open failed";
                file = result.value().lock();
                auto started = file->readAsync(
                    expectedSize,
                    buffer.data(),
                    static_cast<IFileStream::SignedSizeType>(buffer.size()),
                    [this](IFileStream::SignedSizeType bytes)
                    {
                        if (received.fetch_add(bytes) == 0)
                            released.wait_for(5s);
                        return true;
                    }
                );
                if (started.wait_for(replyTimeout) != std::future_status::ready)
                    return ::testing::AssertionFailure() << "readAsync timed out";
                auto contextResult = started.get();
                if (!contextResult.has_value())
                    return ::testing::AssertionFailure() << "readAsync failed";
                context = contextResult.value();
                return ::testing::AssertionSuccess();
            }

            bool waitForEnd(std::chrono::milliseconds limit = 5s) const
            {
                const auto deadline = std::chrono::steady_clock::now() + limit;
                while (!context->hasEnded() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(5ms);
                return context->hasEnded();
            }
        };

        ::testing::AssertionResult readsFileOne(SftpSession& sftp)
        {
            auto opened =
                sftp.openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
            if (opened.wait_for(replyTimeout) != std::future_status::ready)
                return ::testing::AssertionFailure() << "open timed out";
            auto file = opened.get().value().lock();
            std::array<char, 64> buffer{};
            auto read = file->readSome(buffer.data(), buffer.size());
            if (read.wait_for(replyTimeout) != std::future_status::ready)
                return ::testing::AssertionFailure() << "read timed out";
            auto bytes = read.get();
            if (!bytes.has_value())
                return ::testing::AssertionFailure() << "read failed";
            const std::string content{buffer.data(), *bytes};
            if (content != "Fake file content")
                return ::testing::AssertionFailure() << "read '" << content << "'";
            return ::testing::AssertionSuccess();
        }
    }

    TEST_F(SftpTests, CancelledReadWithRequestsInFlightLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedRead read;
        ASSERT_TRUE(read.start(*sftp));

        read.context->cancel();
        read.release.set_value();
        ASSERT_TRUE(read.waitForEnd());

        EXPECT_EQ(read.context->outcome(), AsyncTransferContext::Outcome::Canceled);
        // Answers to the abandoned requests must not be taken for the answers of the next calls.
        EXPECT_TRUE(readsFileOne(*sftp));
    }

    TEST_F(SftpTests, ReadOfAFileShorterThanListedFailsAndLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedRead read;
        ASSERT_TRUE(read.start(*sftp, GatedRead::fileSize * 4));
        read.release.set_value();
        ASSERT_TRUE(read.waitForEnd());

        EXPECT_EQ(read.context->outcome(), AsyncTransferContext::Outcome::Failed);
        ASSERT_TRUE(read.context->error().has_value());
        EXPECT_EQ(read.context->error()->wrapperError, WrapperErrors::UnexpectedEndOfFile);
        EXPECT_EQ(read.received.load(), GatedRead::fileSize);
        EXPECT_TRUE(readsFileOne(*sftp));
    }

    TEST_F(SftpTests, PausedReadResumesAfterOtherCallsOnTheSession)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedRead read;
        ASSERT_TRUE(read.start(*sftp));

        read.context->pause(true);
        read.release.set_value();
        // Other calls read past the paused transfer's answers, which must still reach it.
        EXPECT_TRUE(readsFileOne(*sftp));
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(read.context->hasEnded());

        read.context->pause(false);
        ASSERT_TRUE(read.waitForEnd());
        EXPECT_EQ(read.context->outcome(), AsyncTransferContext::Outcome::Completed);
        EXPECT_EQ(read.received.load(), GatedRead::fileSize);
    }

    TEST_F(SftpTests, ClosingTheSftpSessionWithAPausedReadReturns)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        GatedRead read;
        ASSERT_TRUE(read.start(*sftp));
        read.context->pause(true);
        read.release.set_value();

        auto closed = std::async(
            std::launch::async,
            [&sftp]
            {
                return sftp->close();
            }
        );
        ASSERT_EQ(closed.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(closed.get());
        read.context->cancel();
    }

    TEST_F(SftpTests, DestroyingTheSessionWithAReadInFlightReturns)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        GatedRead read;
        ASSERT_TRUE(read.start(*sftp));
        read.release.set_value();
        sftp.reset();

        auto destroyed = std::async(
            std::launch::async,
            [&session]
            {
                session.reset();
            }
        );
        ASSERT_EQ(destroyed.wait_for(5s), std::future_status::ready);
        // Released with the session, nothing reads into the buffer anymore.
        read.file.reset();
        read.context->cancel();
    }

    namespace
    {
        /**
         * @brief Answers the sftp session received but nobody took, counted on its processing thread.
         */
        std::size_t queuedAnswers(SftpSession& sftp)
        {
            return sftp
                .performPromise(
                    [&sftp]()
                    {
                        std::size_t count = 0;
                        for (auto* entry = static_cast<sftp_session>(sftp)->queue; entry != nullptr; entry = entry->next)
                            ++count;
                        return count;
                    },
                    []
                    {
                        return std::numeric_limits<std::size_t>::max();
                    }
                )
                .get();
        }
    }

    TEST_F(SftpTests, ClosingTheStreamOfACancelledReadTakesTheAnswersInFlight)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedRead read;
        ASSERT_TRUE(read.start(*sftp));

        read.context->cancel();
        // Queued behind the turn the gate holds, the close runs before the transfer sees the cancel.
        auto closed = sftp->performPromise(
            [&read]
            {
                read.file->close();
                return true;
            }
        );
        read.release.set_value();
        ASSERT_EQ(closed.wait_for(5s), std::future_status::ready);
        ASSERT_TRUE(read.waitForEnd());

        EXPECT_TRUE(readsFileOne(*sftp));
        EXPECT_EQ(queuedAnswers(*sftp), 0u);
    }

    namespace
    {
        /**
         * @brief An asynchronous upload of a 64 KiB pattern in pieces of 1 KiB. The local read for the
         *        fourth piece waits until the test has done what it wants to do while three requests are in
         *        flight.
         */
        struct GatedUpload
        {
            static constexpr IFileStream::SignedSizeType fileSize = 65536;
            static constexpr int gatedRead = 4;

            std::array<char, 1024> buffer{};
            std::atomic<IFileStream::SignedSizeType> readLocally{0};
            std::atomic_int reads{0};
            std::atomic_int failingRead{0};
            std::promise<void> release{};
            std::shared_future<void> released{release.get_future().share()};
            std::shared_ptr<FileStream> file{};
            std::shared_ptr<AsyncTransferContext> context{};

            static char patternAt(IFileStream::SignedSizeType offset)
            {
                return static_cast<char>('a' + offset % 23);
            }

            static std::string pattern(IFileStream::SignedSizeType size = fileSize)
            {
                std::string content(static_cast<std::size_t>(size), '\0');
                for (IFileStream::SignedSizeType offset = 0; offset < size; ++offset)
                    content[static_cast<std::size_t>(offset)] = patternAt(offset);
                return content;
            }

            ::testing::AssertionResult start(SftpSession& sftp)
            {
                auto opened = sftp.openFile(
                    remote("/home/test/uploaded.bin"),
                    SftpSession::OpenType::Write | SftpSession::OpenType::Create | SftpSession::OpenType::Truncate,
                    std::filesystem::perms{0644u}
                );
                if (opened.wait_for(replyTimeout) != std::future_status::ready)
                    return ::testing::AssertionFailure() << "open timed out";
                auto result = opened.get();
                if (!result.has_value())
                    return ::testing::AssertionFailure() << "open failed";
                file = result.value().lock();
                auto started = file->writeAsync(
                    fileSize,
                    buffer.data(),
                    static_cast<IFileStream::SignedSizeType>(buffer.size()),
                    [this](IFileStream::SignedSizeType bytes) -> IFileStream::SignedSizeType
                    {
                        const auto call = ++reads;
                        if (call == gatedRead)
                            released.wait_for(5s);
                        if (call == failingRead.load())
                            return -1;
                        const auto offset = readLocally.load();
                        const auto length =
                            std::min({bytes, static_cast<IFileStream::SignedSizeType>(buffer.size()), fileSize - offset});
                        for (IFileStream::SignedSizeType index = 0; index < length; ++index)
                            buffer[static_cast<std::size_t>(index)] = patternAt(offset + index);
                        readLocally += length;
                        return length;
                    }
                );
                if (started.wait_for(replyTimeout) != std::future_status::ready)
                    return ::testing::AssertionFailure() << "writeAsync timed out";
                auto contextResult = started.get();
                if (!contextResult.has_value())
                    return ::testing::AssertionFailure() << "writeAsync failed";
                context = contextResult.value();
                return ::testing::AssertionSuccess();
            }

            bool waitForEnd(std::chrono::milliseconds limit = 5s) const
            {
                const auto deadline = std::chrono::steady_clock::now() + limit;
                while (!context->hasEnded() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(5ms);
                return context->hasEnded();
            }
        };

        std::optional<std::string> remoteContent(SftpSession& sftp, std::string const& path)
        {
            auto opened = sftp.openFile(remote(path), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
            if (opened.wait_for(replyTimeout) != std::future_status::ready)
                return std::nullopt;
            auto result = opened.get();
            if (!result.has_value())
                return std::nullopt;
            auto file = result.value().lock();
            std::string content{};
            auto read = file->readAll(
                [&content](std::string_view data)
                {
                    content += data;
                    return true;
                }
            );
            if (read.wait_for(replyTimeout) != std::future_status::ready || !read.get().has_value())
                return std::nullopt;
            return content;
        }
    }

    TEST_F(SftpTests, PipelinedUploadWritesTheFileInOrder)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        ASSERT_TRUE(upload.start(*sftp));
        upload.release.set_value();
        ASSERT_TRUE(upload.waitForEnd());

        EXPECT_EQ(upload.context->outcome(), AsyncTransferContext::Outcome::Completed);
        EXPECT_EQ(upload.context->bytesTransferred(), GatedUpload::fileSize);
        EXPECT_EQ(remoteContent(*sftp, "/home/test/uploaded.bin"), GatedUpload::pattern());
    }

    TEST_F(SftpTests, CancelledUploadWithRequestsInFlightLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        ASSERT_TRUE(upload.start(*sftp));

        upload.context->cancel();
        upload.release.set_value();
        ASSERT_TRUE(upload.waitForEnd());

        EXPECT_EQ(upload.context->outcome(), AsyncTransferContext::Outcome::Canceled);
        // Answers to the abandoned writes must not be taken for the answers of the next calls.
        EXPECT_TRUE(readsFileOne(*sftp));
    }

    TEST_F(SftpTests, PausedUploadResumesAfterOtherCallsOnTheSession)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        ASSERT_TRUE(upload.start(*sftp));

        upload.context->pause(true);
        upload.release.set_value();
        EXPECT_TRUE(readsFileOne(*sftp));
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(upload.context->hasEnded());

        upload.context->pause(false);
        ASSERT_TRUE(upload.waitForEnd());
        EXPECT_EQ(upload.context->outcome(), AsyncTransferContext::Outcome::Completed);
        EXPECT_EQ(remoteContent(*sftp, "/home/test/uploaded.bin"), GatedUpload::pattern());
    }

    TEST_F(SftpTests, RefusedWriteWithRequestsInFlightFailsAndLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("failWritesAfter", {{"bytes", 2048}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        ASSERT_TRUE(upload.start(*sftp));
        upload.release.set_value();
        ASSERT_TRUE(upload.waitForEnd());

        EXPECT_EQ(upload.context->outcome(), AsyncTransferContext::Outcome::Failed);
        EXPECT_EQ(upload.context->bytesTransferred(), 2048);
        EXPECT_TRUE(readsFileOne(*sftp));
    }

    TEST_F(SftpTests, LocalReadFailureWithRequestsInFlightFailsAndLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        upload.failingRead = GatedUpload::gatedRead;
        ASSERT_TRUE(upload.start(*sftp));
        upload.release.set_value();
        ASSERT_TRUE(upload.waitForEnd());

        EXPECT_EQ(upload.context->outcome(), AsyncTransferContext::Outcome::Failed);
        ASSERT_TRUE(upload.context->error().has_value());
        EXPECT_EQ(upload.context->error()->wrapperError, WrapperErrors::LocalIoFailure);
        EXPECT_TRUE(readsFileOne(*sftp));
    }

    TEST_F(SftpTests, ClosingTheSftpSessionWithAPausedUploadReturns)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        ASSERT_TRUE(upload.start(*sftp));
        upload.context->pause(true);
        upload.release.set_value();

        auto closed = std::async(
            std::launch::async,
            [&sftp]
            {
                return sftp->close();
            }
        );
        ASSERT_EQ(closed.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(closed.get());
        upload.context->cancel();
    }

    TEST_F(SftpTests, DestroyingTheSessionWithAnUploadInFlightReturns)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        GatedUpload upload;
        ASSERT_TRUE(upload.start(*sftp));
        upload.release.set_value();
        sftp.reset();

        auto destroyed = std::async(
            std::launch::async,
            [&session]
            {
                session.reset();
            }
        );
        ASSERT_EQ(destroyed.wait_for(5s), std::future_status::ready);
        upload.file.reset();
        upload.context->cancel();
    }

    TEST_F(SftpTests, ReadWhoseCallbackClosesTheStreamFailsAndLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto opened =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(opened.wait_for(replyTimeout), std::future_status::ready);
        auto file = opened.get().value().lock();
        ASSERT_TRUE(file);

        constexpr IFileStream::SignedSizeType fileSize = 65536;
        std::array<char, 1024> buffer{};
        std::atomic_int calls{0};
        auto started = file->readAsync(
            fileSize,
            buffer.data(),
            static_cast<IFileStream::SignedSizeType>(buffer.size()),
            [&calls, stream = file.get()](IFileStream::SignedSizeType)
            {
                // On the processing thread with requests in flight, the close runs right here.
                if (++calls == 3)
                    stream->close();
                return true;
            }
        );
        ASSERT_EQ(started.wait_for(replyTimeout), std::future_status::ready);
        auto context = started.get().value();

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!context->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        ASSERT_TRUE(context->hasEnded());
        EXPECT_EQ(context->outcome(), AsyncTransferContext::Outcome::Failed);
        ASSERT_TRUE(context->error().has_value());
        EXPECT_EQ(context->error()->wrapperError, WrapperErrors::FileNull);
        EXPECT_EQ(calls.load(), 3);
        EXPECT_TRUE(readsFileOne(*sftp));
        EXPECT_EQ(queuedAnswers(*sftp), 0u);
    }

    TEST_F(SftpTests, UploadWhoseLocalReadClosesTheStreamFailsAndLeavesTheSessionUsable)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto opened = sftp->openFile(
            remote("/home/test/uploaded.bin"),
            SftpSession::OpenType::Write | SftpSession::OpenType::Create | SftpSession::OpenType::Truncate,
            std::filesystem::perms{0644u}
        );
        ASSERT_EQ(opened.wait_for(replyTimeout), std::future_status::ready);
        auto file = opened.get().value().lock();
        ASSERT_TRUE(file);

        constexpr IFileStream::SignedSizeType fileSize = 65536;
        std::array<char, 1024> buffer{};
        std::atomic_int calls{0};
        auto started = file->writeAsync(
            fileSize,
            buffer.data(),
            static_cast<IFileStream::SignedSizeType>(buffer.size()),
            [&calls, &buffer, stream = file.get()](IFileStream::SignedSizeType bytes) -> IFileStream::SignedSizeType
            {
                // Three writes are in flight when the close runs.
                if (++calls == 4)
                    stream->close();
                const auto length = std::min(bytes, static_cast<IFileStream::SignedSizeType>(buffer.size()));
                std::fill_n(buffer.begin(), length, 'x');
                return length;
            }
        );
        ASSERT_EQ(started.wait_for(replyTimeout), std::future_status::ready);
        auto context = started.get().value();

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!context->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        ASSERT_TRUE(context->hasEnded());
        EXPECT_EQ(context->outcome(), AsyncTransferContext::Outcome::Failed);
        ASSERT_TRUE(context->error().has_value());
        EXPECT_EQ(context->error()->wrapperError, WrapperErrors::FileNull);
        EXPECT_EQ(calls.load(), 4);
        EXPECT_TRUE(readsFileOne(*sftp));
        EXPECT_EQ(queuedAnswers(*sftp), 0u);
    }

    TEST_F(SftpTests, TransferObserverHearsOfTheEnd)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto opened =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(opened.wait_for(replyTimeout), std::future_status::ready);
        auto file = opened.get().value().lock();
        ASSERT_TRUE(file);

        constexpr IFileStream::SignedSizeType fileSize = 65536;
        std::array<char, 1024> buffer{};
        std::atomic_int calls{0};
        std::atomic_bool lastSawTheEnd{false};
        std::shared_ptr<AsyncTransferContext> context{};
        // Observed from the processing thread before the first turn, like the operations do.
        auto started = file->strand()->pushPromiseTask(
            [&]()
            {
                auto result = file->readAsyncInStrand(
                    fileSize,
                    buffer.data(),
                    static_cast<IFileStream::SignedSizeType>(buffer.size()),
                    [](IFileStream::SignedSizeType)
                    {
                        return true;
                    }
                );
                if (!result.has_value())
                    return false;
                context = result.value();
                context->observe(
                    [&calls, &lastSawTheEnd, weakContext = std::weak_ptr{context}]()
                    {
                        ++calls;
                        if (auto observed = weakContext.lock(); observed)
                            lastSawTheEnd = observed->hasEnded();
                    }
                );
                return true;
            }
        );
        ASSERT_EQ(started.wait_for(replyTimeout), std::future_status::ready);
        ASSERT_TRUE(started.get());

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!context->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        ASSERT_TRUE(context->hasEnded());
        // The end is reported after the outcome is set, so a woken queue finds the transfer ended.
        while (!lastSawTheEnd && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        EXPECT_TRUE(lastSawTheEnd);
        EXPECT_GE(calls.load(), 1);
        EXPECT_EQ(context->outcome(), AsyncTransferContext::Outcome::Completed);
    }

    TEST_F(SftpTests, MockServerCanFailReads)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("failReadsAfter", {{"bytes", 0}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);

        std::array<char, 64> buffer{};
        auto readFut = file->readSome(buffer.data(), buffer.size());
        ASSERT_EQ(readFut.wait_for(replyTimeout), std::future_status::ready);
        EXPECT_FALSE(readFut.get().has_value());
    }

    TEST_F(SftpTests, MockServerCanDenyDirectoryListing)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Documents"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto listFut = sftp->listDirectory(remote("/home/test/Documents"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        EXPECT_FALSE(listFut.get().has_value());
    }

    TEST_F(SftpTests, MockServerListsRawNamesVerbatim)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("rawName", {{"directory", "/home/test"}, {"name", "x/.."}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        const auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());
        const auto iter = std::find_if(listResult->begin(), listResult->end(), [](auto const& entry) {
            return entry.name == "x/..";
        });
        EXPECT_NE(iter, listResult->end());
    }

    TEST_F(SftpTests, ListedNamesThatAreNotUtf8AreShownButNeverNamed)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("rawName", {{"directory", "/home/test"}, {"nameHex", "61ff62"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        const auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());
        const auto iter = std::find_if(listResult->begin(), listResult->end(), [](auto const& entry) {
            return entry.name == "a\xEF\xBF\xBD"
                                 "b";
        });
        ASSERT_NE(iter, listResult->end());
        EXPECT_FALSE(iter->nameIsValidUtf8);
        const auto segment = iter->nameSegment();
        ASSERT_FALSE(segment.has_value());
        EXPECT_EQ(segment.error().reason, Paths::PathRejection::InvalidUtf8);

        const nlohmann::json listing = *listResult;
        const auto dumped = listing.dump();
        const auto parsed = nlohmann::json::parse(dumped).get<std::vector<SharedData::DirectoryEntry>>();
        const auto parsedIter = std::find_if(parsed.begin(), parsed.end(), [](auto const& entry) {
            return !entry.nameIsValidUtf8;
        });
        ASSERT_NE(parsedIter, parsed.end());
        EXPECT_EQ(parsedIter->name, iter->name);
        EXPECT_EQ(
            std::count_if(parsed.begin(), parsed.end(), [](auto const& entry) {
                return entry.nameIsValidUtf8;
            }),
            static_cast<std::ptrdiff_t>(parsed.size() - 1u)
        );
    }

    TEST_F(SftpTests, MockServerPosixRenameExtensionOverwrites)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "posix-rename@openssh.com"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename(remote("/home/test/file1.txt"), remote("/home/test/file2.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(replyTimeout), std::future_status::ready);
        const auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());
        EXPECT_EQ(std::count_if(listResult->begin(), listResult->end(), [](auto const& entry) {
            return entry.name == "file1.txt" || entry.name == "file2.txt";
        }), 1);
    }

    TEST_F(SftpTests, MockServerPlainRenameDoesNotOverwrite)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename(remote("/home/test/file1.txt"), remote("/home/test/file2.txt"));
        ASSERT_EQ(fut.wait_for(replyTimeout), std::future_status::ready);
        EXPECT_FALSE(fut.get().has_value());
    }

    namespace
    {
        std::string readRemote(SftpSession& sftp, std::string_view path)
        {
            auto openFuture = sftp.openFile(remote(path), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
            if (openFuture.wait_for(replyTimeout) != std::future_status::ready)
                return "<timeout>";
            auto opened = openFuture.get();
            if (!opened.has_value())
                return "<missing>";
            std::string data;
            auto readFuture = opened->lock()->readAll(
                [&data](std::string_view chunk)
                {
                    data.append(chunk);
                    return true;
                }
            );
            if (readFuture.wait_for(replyTimeout) != std::future_status::ready || !readFuture.get().has_value())
                return "<read failed>";
            return data;
        }

        bool remoteExists(SftpSession& sftp, std::string_view path)
        {
            auto future = sftp.lstat(remote(path));
            return future.wait_for(replyTimeout) == std::future_status::ready && future.get().has_value();
        }
    }

    TEST_F(SftpTests, RenameNoReplaceRefusesAnExistingFile)
    {
        for (const bool hardlink : {false, true})
        {
            CREATE_SERVER_AND_JOINER(Sftp);
            ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
            ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
            if (hardlink)
                ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "hardlink@openssh.com"}}));
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            auto future = sftp->performPromise(
                [&sftp]()
                {
                    return sftp->renameNoReplaceInStrand(remote("/home/test/source.txt"), remote("/home/test/target.txt"));
                }
            );
            ASSERT_EQ(future.wait_for(replyTimeout), std::future_status::ready);
            const auto result = future.get();
            ASSERT_FALSE(result.has_value()) << "hardlink: " << hardlink;
            EXPECT_EQ(result.error().sftpError, SSH_FX_FILE_ALREADY_EXISTS);
            EXPECT_EQ(readRemote(*sftp, "/home/test/source.txt"), "source");
            EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "target");
        }
    }

    TEST_F(SftpTests, RenameNoReplaceMovesToAFreeName)
    {
        for (const bool hardlink : {false, true})
        {
            CREATE_SERVER_AND_JOINER(Sftp);
            ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
            if (hardlink)
                ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "hardlink@openssh.com"}}));
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            auto future = sftp->performPromise(
                [&sftp]()
                {
                    return sftp->renameNoReplaceInStrand(remote("/home/test/source.txt"), remote("/home/test/moved.txt"));
                }
            );
            ASSERT_EQ(future.wait_for(replyTimeout), std::future_status::ready);
            ASSERT_TRUE(future.get().has_value()) << "hardlink: " << hardlink;
            EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
            EXPECT_EQ(readRemote(*sftp, "/home/test/moved.txt"), "source");
        }
    }

    TEST_F(SftpTests, RenameReplaceReplacesWithAndWithoutPosixRename)
    {
        for (const bool posixRename : {false, true})
        {
            CREATE_SERVER_AND_JOINER(Sftp);
            ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
            ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
            if (posixRename)
                ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "posix-rename@openssh.com"}}));
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            auto future = sftp->performPromise(
                [&sftp]()
                {
                    return sftp->renameReplaceInStrand(remote("/home/test/source.txt"), remote("/home/test/target.txt"), ".aside");
                }
            );
            ASSERT_EQ(future.wait_for(replyTimeout), std::future_status::ready);
            ASSERT_TRUE(future.get().has_value()) << "posixRename: " << posixRename;
            EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "source");
            EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
            EXPECT_FALSE(remoteExists(*sftp, "/home/test/target.txt.aside"));
        }
    }

    namespace
    {
        std::expected<void, SftpError> renameReplace(SftpSession& sftp)
        {
            auto future = sftp.performPromise(
                [&sftp]()
                {
                    return sftp.renameReplaceInStrand(
                        remote("/home/test/source.txt"), remote("/home/test/target.txt"), ".aside"
                    );
                }
            );
            if (future.wait_for(replyTimeout) != std::future_status::ready)
                return std::unexpected(SftpError{.message = "timeout"});
            return future.get();
        }

        std::expected<void, SftpError> renameNoReplace(
            SftpSession& sftp,
            std::string_view destination,
            std::optional<SharedData::FileType> sourceType = std::nullopt
        )
        {
            auto future = sftp.performPromise(
                [&sftp, destination, sourceType]()
                {
                    return sftp.renameNoReplaceInStrand(remote("/home/test/source.txt"), remote(destination), sourceType);
                }
            );
            if (future.wait_for(replyTimeout) != std::future_status::ready)
                return std::unexpected(SftpError{.message = "timeout"});
            return future.get();
        }
    }

    TEST_F(SftpTests, FailedRenameReplaceRestoresTheDestination)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
        // Moving the destination aside succeeds, the rename into place fails, moving it back succeeds.
        ASSERT_TRUE(serverStartResult->command("failRenames", {{"after", 1}, {"count", 1}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        EXPECT_FALSE(renameReplace(*sftp).has_value());
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "target");
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/target.txt.aside"));
        EXPECT_EQ(readRemote(*sftp, "/home/test/source.txt"), "source");
    }

    TEST_F(SftpTests, FailedRollbackNamesWhereTheDestinationIsKept)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
        ASSERT_TRUE(serverStartResult->command("failRenames", {{"after", 1}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto result = renameReplace(*sftp);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().message.find("/home/test/target.txt.aside"), std::string::npos) << result.error().message;
        ASSERT_TRUE(serverStartResult->command("failRenames", {{"enabled", false}}));
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt.aside"), "target");
        EXPECT_EQ(readRemote(*sftp, "/home/test/source.txt"), "source");
    }

    TEST_F(SftpTests, RenameReplaceRemovesALeftoverAside)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt.aside"}, {"content", "old"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ASSERT_TRUE(renameReplace(*sftp).has_value());
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "source");
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/target.txt.aside"));
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
    }

    TEST_F(SftpTests, RenameReplacePutsBackADestinationThatOnlyExistsAside)
    {
        for (const bool failInPlace : {false, true})
        {
            CREATE_SERVER_AND_JOINER(Sftp);
            ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
            ASSERT_TRUE(
                serverStartResult->command("createFile", {{"path", "/home/test/target.txt.aside"}, {"content", "target"}})
            );
            // Putting it back and moving it aside again succeed, the rename into place fails.
            if (failInPlace)
                ASSERT_TRUE(serverStartResult->command("failRenames", {{"after", 2}, {"count", 1}}));
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            const auto result = renameReplace(*sftp);
            EXPECT_EQ(result.has_value(), !failInPlace);
            EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), failInPlace ? "target" : "source");
            EXPECT_FALSE(remoteExists(*sftp, "/home/test/target.txt.aside")) << "failInPlace: " << failInPlace;
        }
    }

    TEST_F(SftpTests, RenameNoReplaceRenamesWhenTheServerRefusesHardlinks)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
        ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "hardlink@openssh.com"}}));
        ASSERT_TRUE(serverStartResult->command("failHardlinks", nlohmann::json::object()));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto refused = renameNoReplace(*sftp, "/home/test/target.txt", SharedData::FileType::Regular);
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().sftpError, SSH_FX_FILE_ALREADY_EXISTS);
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "target");

        ASSERT_TRUE(renameNoReplace(*sftp, "/home/test/moved.txt", SharedData::FileType::Regular).has_value());
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
        EXPECT_EQ(readRemote(*sftp, "/home/test/moved.txt"), "source");
    }

    TEST_F(SftpTests, RenameNoReplaceSucceedsWhenTheLinkedSourceCannotBeRemoved)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "hardlink@openssh.com"}}));
        ASSERT_TRUE(serverStartResult->command("failRemoves", nlohmann::json::object()));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ASSERT_TRUE(renameNoReplace(*sftp, "/home/test/moved.txt").has_value());
        EXPECT_EQ(readRemote(*sftp, "/home/test/moved.txt"), "source");
    }

    TEST_F(SftpTests, MoveIntoPlaceReplacesOnlyWhenAllowed)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto move = [&sftp](bool replace)
        {
            auto future = sftp->performPromise(
                [&sftp, replace]()
                {
                    return sftp->moveIntoPlaceInStrand(
                        remote("/home/test/source.txt"), remote("/home/test/target.txt"), replace, ".aside"
                    );
                }
            );
            return future.get();
        };
        const auto refused = move(false);
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().sftpError, SSH_FX_FILE_ALREADY_EXISTS);
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "target");

        ASSERT_TRUE(move(true).has_value());
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "source");
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
    }
}
