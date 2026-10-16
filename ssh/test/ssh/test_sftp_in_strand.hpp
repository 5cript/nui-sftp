#pragma once

#include "common_fixture.hpp"
#include "test_sftp.hpp"

#include <ssh/session.hpp>
#include <ssh/sftp_session.hpp>
#include <ssh/file_stream.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace SecureShell::Test
{
    class SftpInStrandTests : public SftpTests
    {
      public:
        template <typename FunctionT>
        auto runInStrand(std::shared_ptr<SftpSession> const& sftp, FunctionT&& func)
        {
            auto fut = sftp->performPromise(std::forward<FunctionT>(func));
            EXPECT_EQ(fut.wait_for(5s), std::future_status::ready);
            return fut.get();
        }
    };

    TEST_F(SftpInStrandTests, ListDirectoryInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->listDirectoryInStrand(remote("/home/test"));
        });
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_GT(result.value().size(), 0u);
    }

    TEST_F(SftpInStrandTests, CreateDirectoryInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->createDirectoryInStrand(remote("/home/test/newdir_is"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, CreateDirectoryIfItDoesntExistInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        // Existing directory — should be a no-op success.
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->createDirectoryIfItDoesntExistInStrand(remote("/home/test"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, CreateFileInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->createFileInStrand(remote("/home/test/newfile_is.txt"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, RemoveFileInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->removeFileInStrand(remote("/home/test/file1.txt"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, RemoveDirectoryInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        // First create a directory we can remove.
        auto mk = runInStrand(sftp, [sftp = sftp]() {
            return sftp->createDirectoryInStrand(remote("/home/test/to_remove_is"));
        });
        ASSERT_TRUE(mk.has_value());
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->removeDirectoryInStrand(remote("/home/test/to_remove_is"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, StatInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->statInStrand(remote("/home/test/file1.txt"));
        });
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_GT(result.value().size, 0u);
    }

    TEST_F(SftpInStrandTests, LstatInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->lstatInStrand(remote("/home/test/file1.txt"));
        });
        ASSERT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, RenameInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->renameInStrand(remote("/home/test/file1.txt"), remote("/home/test/file1_renamed_is.txt"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, ChmodInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->chmodInStrand(remote("/home/test/file1.txt"), std::filesystem::perms::owner_all);
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, LimitsInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->limitsInStrand();
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, OpenFileInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->openFileInStrand(
                remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
            );
        });
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(result.value().expired());
    }

    TEST_F(SftpInStrandTests, ReadLinkDeepInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        // Non-symlink regular file is a valid target.
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->readLinkDeepInStrand(remote("/home/test/file1.txt"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(SftpInStrandTests, ReadLinkDeepResolvesRelativeLinksFromTheLinkDirectory)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(serverStartResult->command(
            "createSymlink", {{"path", "/home/test/Documents/relative_link"}, {"target", "../link_to_file1.txt"}}
        ));
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->readLinkDeepInStrand(remote("/home/test/Documents/relative_link"));
        });
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(result->linkContent, "../link_to_file1.txt");
        EXPECT_EQ(result->linkTarget.value(), "/home/test/file1.txt");
        ASSERT_TRUE(result->targetInfo.has_value());
        EXPECT_TRUE(result->targetInfo->isRegularFile());
    }

    TEST_F(SftpInStrandTests, UnsetPathsNeverReachTheServer)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto removed = runInStrand(sftp, [sftp = sftp]() {
            return sftp->removeFileInStrand(Paths::RemotePath{});
        });
        ASSERT_FALSE(removed.has_value());
        EXPECT_EQ(removed.error().wrapperError, WrapperErrors::InvalidPath);

        auto renamed = runInStrand(sftp, [sftp = sftp]() {
            return sftp->renameReplaceInStrand(remote("/home/test/file1.txt"), Paths::RemotePath{}, ".old");
        });
        ASSERT_FALSE(renamed.has_value());
        EXPECT_EQ(renamed.error().wrapperError, WrapperErrors::InvalidPath);

        auto stillThere = runInStrand(sftp, [sftp = sftp]() {
            return sftp->lstatInStrand(remote("/home/test/file1.txt"));
        });
        EXPECT_TRUE(stillThere.has_value());
    }

    TEST_F(SftpInStrandTests, CreateSymLinkInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto result = runInStrand(sftp, [sftp = sftp]() {
            return sftp->createSymLinkInStrand("/home/test/file1.txt", remote("/home/test/link_is.txt"));
        });
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    // ------- FileStream happy paths -------

    TEST_F(SftpInStrandTests, FileStreamSeekTellStatRewindReadSomeInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto opened = fut.get();
        ASSERT_TRUE(opened.has_value());
        auto file = opened.value().lock();
        ASSERT_TRUE(file);

        auto done = sftp->performPromise([file = file]() -> bool {
            EXPECT_TRUE(file->seekInStrand(2).has_value());
            auto tellRes = file->tellInStrand();
            EXPECT_TRUE(tellRes.has_value());
            EXPECT_EQ(tellRes.value(), 2u);
            EXPECT_TRUE(file->statInStrand().has_value());
            EXPECT_TRUE(file->rewindInStrand().has_value());
            char byte = 0;
            auto readRes = file->readSomeInStrand(&byte, 1);
            EXPECT_TRUE(readRes.has_value());
            EXPECT_EQ(byte, 'F');
            return true;
        });
        ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(done.get());
    }

    TEST_F(SftpInStrandTests, FileStreamCloseInStrandHappyPath)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto opened = fut.get();
        ASSERT_TRUE(opened.has_value());
        auto file = opened.value().lock();
        ASSERT_TRUE(file);

        auto done = sftp->performPromise([file = file]() -> bool {
            file->closeInStrand(false);
            return true;
        });
        ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(done.get());
    }

    // ------- Death tests: asserts fire when called outside the strand -------

    class SftpInStrandDeathTests : public SftpInStrandTests
    {};

    TEST_F(SftpInStrandDeathTests, ListDirectoryInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->listDirectoryInStrand(remote("/home/test")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, CreateDirectoryInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->createDirectoryInStrand(remote("/home/test/x_dt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, CreateDirectoryIfItDoesntExistInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->createDirectoryIfItDoesntExistInStrand(remote("/home/test/x_dt2")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, CreateFileInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->createFileInStrand(remote("/home/test/x_dt.txt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, RemoveFileInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->removeFileInStrand(remote("/home/test/file1.txt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, RemoveDirectoryInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->removeDirectoryInStrand(remote("/home/test/Documents")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, StatInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->statInStrand(remote("/home/test/file1.txt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, LstatInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->lstatInStrand(remote("/home/test/file1.txt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, SetstatInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        sftp_attributes_struct attrs{};
        ASSERT_DEBUG_DEATH({ (void)sftp->statInStrand(remote("/home/test/file1.txt"), &attrs); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, RenameInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->renameInStrand(remote("/home/test/file1.txt"), remote("/home/test/whatever.txt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, ChownInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->chownInStrand(remote("/home/test/file1.txt"), 0, 0); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, ChmodInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH(
            { (void)sftp->chmodInStrand(remote("/home/test/file1.txt"), std::filesystem::perms::owner_all); }, ".*"
        );
    }

    TEST_F(SftpInStrandDeathTests, LimitsInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->limitsInStrand(); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, OpenFileInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH(
            {
                (void)sftp->openFileInStrand(
                    remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
                );
            },
            ".*"
        );
    }

    TEST_F(SftpInStrandDeathTests, ReadLinkDeepInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH({ (void)sftp->readLinkDeepInStrand(remote("/home/test/file1.txt")); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, CreateSymLinkInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_DEBUG_DEATH(
            { (void)sftp->createSymLinkInStrand("/home/test/file1.txt", remote("/home/test/link_dt.txt")); }, ".*"
        );
    }

    // FileStream death tests
    //
    // For these we open the file via the public (async) API first, then call the InStrand method
    // from the test thread — which is not the processing thread, so the assert must fire.

    TEST_F(SftpInStrandDeathTests, FileStreamSeekInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);
        ASSERT_DEBUG_DEATH({ (void)file->seekInStrand(0); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, FileStreamTellInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);
        ASSERT_DEBUG_DEATH({ (void)file->tellInStrand(); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, FileStreamStatInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);
        ASSERT_DEBUG_DEATH({ (void)file->statInStrand(); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, FileStreamRewindInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);
        ASSERT_DEBUG_DEATH({ (void)file->rewindInStrand(); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, FileStreamReadSomeInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);
        char buf = 0;
        ASSERT_DEBUG_DEATH({ (void)file->readSomeInStrand(&buf, 1); }, ".*");
    }

    TEST_F(SftpInStrandDeathTests, FileStreamCloseInStrandAssertsOutsideStrand)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        auto fut = sftp->openFile(
            remote("/home/test/file1.txt"), SftpSession::OpenType::Read, std::filesystem::perms::owner_read
        );
        ASSERT_EQ(fut.wait_for(1s), std::future_status::ready);
        auto file = fut.get().value().lock();
        ASSERT_TRUE(file);
        ASSERT_DEBUG_DEATH({ file->closeInStrand(false); }, ".*");
    }
}
