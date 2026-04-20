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
            if (std::filesystem::exists(programDirectory / "temp" / "log.txt"))
            {
                if (!std::filesystem::exists(programDirectory / "logs"))
                    std::filesystem::create_directory(programDirectory / "logs");

                std::filesystem::rename(
                    programDirectory / "temp" / "log.txt",
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

        auto fut = sftp->listDirectory("/home/test");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_GT(result.value().size(), 0);
        auto it = std::find_if(result.value().begin(), result.value().end(), [](const auto& entry) {
            return entry.path == "file1.txt";
        });
        EXPECT_NE(it, result.value().end());
    }

    TEST_F(SftpTests, ListingNonExistantDirectoryMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->listDirectory("/home/test/nonexistant");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanCreateDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createDirectory("/home/test/newdir");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory("/home/test");
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "newdir";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CreatingDirectoryThatAlreadyExistsMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createDirectory("/home/test");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanCreateFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile("/home/test/newfile.txt");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory("/home/test");
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "newfile.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CreatingFileInNonExistantDirectoryMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->createFile("/home/test/nonexistant/newfile.txt");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanRemoveFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->removeFile("/home/test/file1.txt");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory("/home/test");
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "file1.txt";
        });
        EXPECT_EQ(it, listResult.value().end());
    }

    TEST_F(SftpTests, RemovingNonExistantFileMayFail)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->removeFile("/home/test/nonexistant.txt");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(SftpTests, CanStatFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->stat("/home/test/file1.txt");
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        EXPECT_GT(result.value().size, 0);
    }

    TEST_F(SftpTests, CanRenameFile)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename("/home/test/file1.txt", "/home/test/file1_renamed.txt");
        ASSERT_EQ(fut.wait_for(3s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value()) << result.error().message;

        auto listFut = sftp->listDirectory("/home/test");
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "file1_renamed.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CanRenameDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->rename("/home/test/Documents", "/home/test/Documents_renamed");
        ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
        auto result = fut.get();
        ASSERT_TRUE(result.has_value());

        auto listFut = sftp->listDirectory("/home/test");
        ASSERT_EQ(listFut.wait_for(1s), std::future_status::ready);
        auto listResult = listFut.get();
        ASSERT_TRUE(listResult.has_value());

        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "Documents_renamed";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(SftpTests, CanReadFilePartly)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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

    TEST_F(SftpTests, CanReadBigFileChunkMethod)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto fut =
            sftp->openFile("/home/test/large.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
                sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/file1.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/large.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/large.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/large.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
            sftp->openFile("/home/test/large.txt", SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
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
}
