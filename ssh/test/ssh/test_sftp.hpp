#pragma once

#include "common_fixture.hpp"
#include <ssh/session.hpp>
#include <nui/utility/scope_exit.hpp>
#include <ssh/sftp_session.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/async/transfer_context.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace SecureShell::Test
{
    class SftpTests : public CommonFixture
    {
      public:
        static constexpr std::chrono::seconds connectTimeout{2};

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
        ASSERT_TRUE(sftpFuture.wait_for(1s) == std::future_status::ready);
        auto sftp = sftpFuture.get();
        EXPECT_TRUE(sftp.has_value());
    }

    TEST_F(SftpTests, CanListDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanCreateDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createDirectory(remote("/home/test/newdir"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanCreateFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile(remote("/home/test/newfile.txt"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.name == "newfile.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CreatingFileInNonExistantDirectoryMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile(remote("/home/test/nonexistant/newfile.txt"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanRemoveFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->removeFile(remote("/home/test/file1.txt"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanStatFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->stat(remote("/home/test/file1.txt"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        EXPECT_GT(result.value().size, 0);
    }

    TEST_F(SftpTests, HomeDirectoryIsLoginDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->homeDirectory();
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/Users/test");

        auto listFut = sftp->listDirectory(*result);
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/home/test");

        auto listFut = sftp->listDirectory(*result);
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->value(), "/");
    }

    TEST_F(SftpTests, CanReadUserDirectories)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->userDirectories(remote("/home/deutsch"));
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().toString();

        std::vector<std::pair<std::string, std::string>> directories;
        for (auto const& directory : *result)
            directories.emplace_back(directory.name, directory.path.generic_string());
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(result->empty());

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        EXPECT_TRUE(listFut.get().has_value());
    }

    TEST_F(SftpTests, CanRenameFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename(remote("/home/test/file1.txt"), remote("/home/test/file1_renamed.txt"));
        ASSERT_EQ(fut.wait_for(3s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        char byte;
        auto readFut = file->readSome(&byte, 1);
        ASSERT_EQ(readFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::vector<char> buffer(1024);
        auto readFut = file->readSome(buffer.data(), buffer.size());

        ASSERT_EQ(readFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        ASSERT_EQ(file->seek(2).wait_for(1s), std::future_status::ready);
        std::vector<char> buffer(1024);
        auto readFut2 = file->readSome(buffer.data(), buffer.size());

        ASSERT_EQ(readFut2.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::vector<char> buffer(1024);
        ASSERT_EQ(file->seek(2).wait_for(1s), std::future_status::ready);
        ASSERT_EQ(file->rewind().wait_for(1s), std::future_status::ready);
        auto readFut2 = file->readSome(buffer.data(), buffer.size());

        ASSERT_EQ(readFut2.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        ASSERT_EQ(file->seek(2).wait_for(1s), std::future_status::ready);
        auto tellFut = file->tell();

        ASSERT_EQ(tellFut.wait_for(1s), std::future_status::ready);
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
            ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto fileWeak = std::move(result).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        auto statFut = file->stat();
        ASSERT_EQ(statFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(ctxFut.wait_for(1s), std::future_status::ready);
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

    TEST_F(SftpTests, AsyncReadCanBePaused)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/large.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = std::move(result).value().lock();
        ASSERT_TRUE(file);

        constexpr std::size_t totalSize = 65536;
        using SignedSize = SecureShell::IFileStream::SignedSizeType;

        std::array<char, 1024> buffer{};
        std::atomic<SignedSize> readCount{0};

        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&readCount](SignedSize bytesRead)
            {
                readCount += bytesRead;
                return true;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(1s), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();

        ctx->pause(true);
        std::this_thread::sleep_for(200ms);
        const auto snapshot = ctx->bytesTransferred();
        std::this_thread::sleep_for(200ms);
        EXPECT_EQ(ctx->bytesTransferred(), snapshot);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());
        auto file = std::move(result).value().lock();
        ASSERT_TRUE(file);

        constexpr std::size_t totalSize = 65536;
        using SignedSize = SecureShell::IFileStream::SignedSizeType;

        std::array<char, 1024> buffer{};
        std::atomic<SignedSize> readCount{0};

        auto ctxFut = file->readAsync(
            static_cast<SignedSize>(totalSize),
            buffer.data(),
            static_cast<SignedSize>(buffer.size()),
            [&readCount](SignedSize bytesRead)
            {
                readCount += bytesRead;
                return true;
            }
        );
        ASSERT_EQ(ctxFut.wait_for(1s), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();

        ctx->pause(true);
        ctx->cancel();

        EXPECT_TRUE(ctx->hasEnded());

        std::this_thread::sleep_for(200ms);
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
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(ctxFut.wait_for(1s), std::future_status::ready);
        auto ctxResult = ctxFut.get();
        ASSERT_TRUE(ctxResult.has_value());
        auto ctx = std::move(ctxResult).value();

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!ctx->hasEnded() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        EXPECT_TRUE(ctx->hasEnded());
        EXPECT_EQ(callCount.load(), 3);
        EXPECT_LT(ctx->bytesTransferred(), static_cast<SignedSize>(totalSize));
    }

    TEST_F(SftpTests, MockServerCanFailReads)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("failReadsAfter", {{"bytes", 0}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile(remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);

        std::array<char, 64> buffer{};
        auto readFut = file->readSome(buffer.data(), buffer.size());
        ASSERT_EQ(readFut.wait_for(1s), std::future_status::ready);
        EXPECT_FALSE(readFut.get().has_value());
    }

    TEST_F(SftpTests, MockServerCanDenyDirectoryListing)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Documents"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto listFut = sftp->listDirectory(remote("/home/test/Documents"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        EXPECT_FALSE(listFut.get().has_value());
    }

    TEST_F(SftpTests, MockServerListsRawNamesVerbatim)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("rawName", {{"directory", "/home/test"}, {"name", "x/.."}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        const auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());
        const auto iter = std::find_if(listResult->begin(), listResult->end(), [](auto const& entry) {
            return entry.name == "x/..";
        });
        EXPECT_NE(iter, listResult->end());
    }

    TEST_F(SftpTests, MockServerPosixRenameExtensionOverwrites)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "posix-rename@openssh.com"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename(remote("/home/test/file1.txt"), remote("/home/test/file2.txt"));
        ASSERT_EQ(fut.wait_for(3s), std::future_status::ready);
        const auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;

        auto listFut = sftp->listDirectory(remote("/home/test"));
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
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
        ASSERT_EQ(fut.wait_for(3s), std::future_status::ready);
        EXPECT_FALSE(fut.get().has_value());
    }

    namespace
    {
        std::string readRemote(SftpSession& sftp, std::string_view path)
        {
            auto openFuture = sftp.openFile(remote(path), SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
            if (openFuture.wait_for(2s) != std::future_status::ready)
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
            if (readFuture.wait_for(2s) != std::future_status::ready || !readFuture.get().has_value())
                return "<read failed>";
            return data;
        }

        bool remoteExists(SftpSession& sftp, std::string_view path)
        {
            auto future = sftp.lstat(remote(path));
            return future.wait_for(2s) == std::future_status::ready && future.get().has_value();
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
            ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
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
            ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
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
            ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
            ASSERT_TRUE(future.get().has_value()) << "posixRename: " << posixRename;
            EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "source");
            EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
            EXPECT_FALSE(remoteExists(*sftp, "/home/test/target.txt.aside"));
        }
    }

    TEST_F(SftpTests, FailedRenameReplaceRestoresTheDestination)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}}));
        // Moving the destination aside succeeds, the rename into place fails.
        ASSERT_TRUE(serverStartResult->command("failRenames", {{"after", 1}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto future = sftp->performPromise(
            [&sftp]()
            {
                return sftp->renameReplaceInStrand(remote("/home/test/source.txt"), remote("/home/test/target.txt"), ".aside");
            }
        );
        ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
        EXPECT_FALSE(future.get().has_value());
        ASSERT_TRUE(serverStartResult->command("failRenames", {{"enabled", false}}));
        // The restore is a rename as well and ran while renames failed, so the original sits aside: never lost.
        const bool restored = readRemote(*sftp, "/home/test/target.txt") == "target";
        const bool keptAside = readRemote(*sftp, "/home/test/target.txt.aside") == "target";
        EXPECT_TRUE(restored || keptAside);
        EXPECT_EQ(readRemote(*sftp, "/home/test/source.txt"), "source");
    }
}
