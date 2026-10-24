#pragma once

#include <backend/sftp/local_file_problem.hpp>
#include <backend/sftp/upload_operation.hpp>
#include "real_server_tests.hpp"

#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <unistd.h>
#endif

#include <cerrno>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Test
{
    using SharedData::LocalFileProblem;

    TEST(LocalFileProblemTests, ABusyFileIsLocked)
    {
        EXPECT_EQ(
            localFileProblemOf(std::make_error_code(std::errc::device_or_resource_busy)), LocalFileProblem::Locked
        );
        EXPECT_EQ(localFileProblemOf(std::make_error_code(std::errc::text_file_busy)), LocalFileProblem::Locked);
    }

    TEST(LocalFileProblemTests, AccessAndPermissionErrorsAreDenied)
    {
        EXPECT_EQ(
            localFileProblemOf(std::make_error_code(std::errc::permission_denied)), LocalFileProblem::PermissionDenied
        );
        EXPECT_EQ(
            localFileProblemOf(std::make_error_code(std::errc::operation_not_permitted)),
            LocalFileProblem::PermissionDenied
        );
    }

    TEST(LocalFileProblemTests, AMissingFileIsNotFound)
    {
        EXPECT_EQ(
            localFileProblemOf(std::make_error_code(std::errc::no_such_file_or_directory)), LocalFileProblem::NotFound
        );
    }

    TEST(LocalFileProblemTests, AFullDiskIsNoSpace)
    {
        EXPECT_EQ(localFileProblemOf(std::make_error_code(std::errc::no_space_on_device)), LocalFileProblem::NoSpace);
    }

    TEST(LocalFileProblemTests, AnyOtherErrorIsOther)
    {
        EXPECT_EQ(localFileProblemOf(std::make_error_code(std::errc::io_error)), LocalFileProblem::Other);
        EXPECT_EQ(localFileProblemOf(std::make_error_code(std::errc::is_a_directory)), LocalFileProblem::Other);
        EXPECT_EQ(localFileProblemOf(std::make_error_code(std::errc::broken_pipe)), LocalFileProblem::Other);
        EXPECT_EQ(localFileProblemOf(std::make_error_code(std::io_errc::stream)), LocalFileProblem::Other);
    }

    TEST(LocalFileProblemTests, NoErrorIsOther)
    {
        EXPECT_EQ(localFileProblemOf(std::error_code{}), LocalFileProblem::Other);
    }

    TEST(LocalFileProblemTests, WindowsCodesAreClassified)
    {
        using LocalFileProblemDetail::windowsFileProblemOf;
        const auto sharingViolation = 32;
        const auto lockViolation = 33;
        const auto accessDenied = 5;
        const auto fileNotFound = 2;
        const auto pathNotFound = 3;
        const auto diskFull = 112;
        const auto invalidHandle = 6;
        const auto invalidData = 13;

        EXPECT_EQ(windowsFileProblemOf(sharingViolation), LocalFileProblem::Locked);
        EXPECT_EQ(windowsFileProblemOf(lockViolation), LocalFileProblem::Locked);
        EXPECT_EQ(windowsFileProblemOf(accessDenied), LocalFileProblem::PermissionDenied);
        EXPECT_EQ(windowsFileProblemOf(fileNotFound), LocalFileProblem::NotFound);
        EXPECT_EQ(windowsFileProblemOf(pathNotFound), LocalFileProblem::NotFound);
        EXPECT_EQ(windowsFileProblemOf(diskFull), LocalFileProblem::NoSpace);
        EXPECT_EQ(windowsFileProblemOf(invalidHandle), LocalFileProblem::Other);
        EXPECT_EQ(windowsFileProblemOf(invalidData), LocalFileProblem::Other);
        EXPECT_EQ(windowsFileProblemOf(0), LocalFileProblem::Other);
    }

#ifdef _WIN32
    TEST(LocalFileProblemTests, SystemErrorsAreWindowsCodesOnWindows)
    {
        const auto sharingViolation = 32;
        const auto lockViolation = 33;
        const auto accessDenied = 5;
        const auto pathNotFound = 3;
        const auto diskFull = 112;
        // EACCES as an errno value, no permission problem as a Windows code.
        const auto invalidData = 13;

        EXPECT_EQ(localFileProblemOf({sharingViolation, std::system_category()}), LocalFileProblem::Locked);
        EXPECT_EQ(localFileProblemOf({lockViolation, std::system_category()}), LocalFileProblem::Locked);
        EXPECT_EQ(localFileProblemOf({accessDenied, std::system_category()}), LocalFileProblem::PermissionDenied);
        EXPECT_EQ(localFileProblemOf({pathNotFound, std::system_category()}), LocalFileProblem::NotFound);
        EXPECT_EQ(localFileProblemOf({diskFull, std::system_category()}), LocalFileProblem::NoSpace);
        EXPECT_EQ(localFileProblemOf({invalidData, std::system_category()}), LocalFileProblem::Other);
    }
#else
    TEST(LocalFileProblemTests, SystemErrorsAreErrnoValuesElsewhere)
    {
        EXPECT_EQ(localFileProblemOf({EBUSY, std::system_category()}), LocalFileProblem::Locked);
        EXPECT_EQ(localFileProblemOf({ETXTBSY, std::system_category()}), LocalFileProblem::Locked);
        EXPECT_EQ(localFileProblemOf({EACCES, std::system_category()}), LocalFileProblem::PermissionDenied);
        EXPECT_EQ(localFileProblemOf({EPERM, std::system_category()}), LocalFileProblem::PermissionDenied);
        EXPECT_EQ(localFileProblemOf({ENOENT, std::system_category()}), LocalFileProblem::NotFound);
        EXPECT_EQ(localFileProblemOf({ENOSPC, std::system_category()}), LocalFileProblem::NoSpace);
    }

    TEST(LocalFileProblemTests, TheNumbersOfWindowsCodesAreErrnoValuesElsewhere)
    {
        const auto sharingViolation = 32;
        const auto lockViolation = 33;
        const auto accessDenied = 5;
        const auto diskFull = 112;

        EXPECT_EQ(localFileProblemOf({sharingViolation, std::system_category()}), LocalFileProblem::Other);
        EXPECT_EQ(localFileProblemOf({lockViolation, std::system_category()}), LocalFileProblem::Other);
        EXPECT_EQ(localFileProblemOf({accessDenied, std::system_category()}), LocalFileProblem::Other);
        EXPECT_EQ(localFileProblemOf({diskFull, std::system_category()}), LocalFileProblem::Other);
    }
#endif

    class LocalFileProblemUploadTests : public RealServerTests
    {
      protected:
        /**
         * @brief Keeps a file from being opened for reading while it lives: on Windows another handle holds it
         *        without sharing, elsewhere it has no permissions.
         */
        class UnreadableFile
        {
          public:
            explicit UnreadableFile(std::filesystem::path path)
                : path_{std::move(path)}
            {
#ifdef _WIN32
                const auto handle = ::CreateFileW(
                    path_.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
                );
                holder_.reset(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
#else
                std::filesystem::permissions(path_, std::filesystem::perms::none);
#endif
            }
            ~UnreadableFile()
            {
#ifndef _WIN32
                std::error_code errorCode{};
                std::filesystem::permissions(path_, std::filesystem::perms::owner_all, errorCode);
#endif
            }
            UnreadableFile(UnreadableFile const&) = delete;
            UnreadableFile& operator=(UnreadableFile const&) = delete;
            UnreadableFile(UnreadableFile&&) = delete;
            UnreadableFile& operator=(UnreadableFile&&) = delete;

          private:
#ifdef _WIN32
            struct HandleCloser
            {
                void operator()(HANDLE handle) const
                {
                    ::CloseHandle(handle);
                }
            };
            std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser> holder_{};
#endif
            std::filesystem::path path_;
        };

        /** @brief What an UnreadableFile is to an upload. */
#ifdef _WIN32
        static constexpr auto unreadableProblem = LocalFileProblem::Locked;
#else
        static constexpr auto unreadableProblem = LocalFileProblem::PermissionDenied;
#endif

        /**
         * @brief Whether this process reads any file, so UnreadableFile cannot keep it from one.
         */
        static bool readsAnyFile()
        {
#ifdef _WIN32
            return false;
#else
            return ::geteuid() == 0;
#endif
        }

        static void writeSource(std::filesystem::path const& path, std::string const& content)
        {
            std::ofstream{path, std::ios_base::binary | std::ios_base::trunc} << content;
        }

        /**
         * @brief Works @p operation until it completes or fails.
         */
        static std::expected<UploadOperation::WorkStatus, Operation::Error>
        workUntilEnded(UploadOperation& operation, int maxIterations = 500)
        {
            auto status = UploadOperation::WorkStatus::MoreWork;
            for (
                int iteration = 0; iteration < maxIterations && status != UploadOperation::WorkStatus::Complete;
                ++iteration
            )
            {
                const auto worked = operation.work();
                if (!worked)
                    return worked;
                status = *worked;
            }
            return status;
        }

        Utility::TemporaryDirectory sources_{temporaryRoot, true};
    };

    TEST_F(LocalFileProblemUploadTests, AnUploadOfAFileItCannotOpenTellsTheLocalProblem)
    {
        if (readsAnyFile())
            GTEST_SKIP() << "root reads any file";
        const auto source = sources_.path() / "unreadable.txt";
        writeSource(source, "unreadable");
        const UnreadableFile unreadable{source};
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        UploadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test/unreadable.txt"),
                .localPath = toLocal(source),
            }
        };

        const auto worked = operation.work();

        ASSERT_FALSE(worked.has_value());
        EXPECT_EQ(worked.error().type, SharedData::OperationErrorType::OpenFailure) << worked.error().toString();
        EXPECT_EQ(worked.error().localProblem, unreadableProblem) << worked.error().toString();
        EXPECT_TRUE(worked.error().extraInfo.has_value());
    }

    TEST_F(LocalFileProblemUploadTests, AnUploadOfAMissingFileTellsItIsNotFound)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        UploadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test/missing.txt"),
                .localPath = toLocal(sources_.path() / "missing.txt"),
            }
        };

        const auto worked = operation.work();

        ASSERT_FALSE(worked.has_value());
        EXPECT_EQ(worked.error().type, SharedData::OperationErrorType::FileNotFound);
        EXPECT_EQ(worked.error().localProblem, LocalFileProblem::NotFound);
    }

    TEST_F(LocalFileProblemUploadTests, AFileThatShrinksWhileItIsSentInChunksFailsWithALocalProblem)
    {
        const auto source = sources_.path() / "shrinking.bin";
        writeSource(source, std::string(1024u * 1024u, 's'));
        bool shrunk = false;
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        UploadOperation operation{
            sftp,
            {
                .progressCallback =
                    [&source, &shrunk](auto, auto, auto, auto)
                {
                    std::error_code errorCode{};
                    if (!std::exchange(shrunk, true))
                        std::filesystem::resize_file(source, 0, errorCode);
                },
                .remotePath = remote("/home/test/shrinking.bin"),
                .localPath = toLocal(source),
            }
        };

        const auto ended = workUntilEnded(operation);

        EXPECT_TRUE(shrunk);
        ASSERT_FALSE(ended.has_value());
        EXPECT_EQ(ended.error().type, SharedData::OperationErrorType::SourceFileNotGood) << ended.error().toString();
        EXPECT_EQ(ended.error().localProblem, LocalFileProblem::Other);
    }

    TEST_F(LocalFileProblemUploadTests, AFileThatShrinksBeforeABigTransferReadsItFailsWithALocalProblem)
    {
        const auto source = sources_.path() / "shrinking.bin";
        writeSource(source, std::string(256u * 1024u, 's'));
        bool shrunk = false;
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [session, sftp] = createSftpSession(serverStartResult->port);
        UploadOperation operation{
            sftp,
            {
                // The first progress comes on the strand right after the transfer started, before it read anything.
                .progressCallback =
                    [&source, &shrunk](auto, auto, auto, auto)
                {
                    std::error_code errorCode{};
                    if (!std::exchange(shrunk, true))
                        std::filesystem::resize_file(source, 0, errorCode);
                },
                .remotePath = remote("/home/test/shrinking.bin"),
                .localPath = toLocal(source),
                .bigFileOptimized = true,
            }
        };

        const auto ended = workUntilEnded(operation);

        EXPECT_TRUE(shrunk);
        ASSERT_FALSE(ended.has_value());
        EXPECT_EQ(ended.error().type, SharedData::OperationErrorType::SourceFileNotGood) << ended.error().toString();
        EXPECT_EQ(ended.error().localProblem, LocalFileProblem::Other);
    }
}
