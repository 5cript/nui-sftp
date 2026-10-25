#pragma once

#include <utility/shared_input_file_stream.hpp>
#include <utility/temporary_directory.hpp>

#include <nui/utility/scope_exit.hpp>

#include <gtest/gtest.h>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <unistd.h>
#endif

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class SharedInputFileStreamTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            content_.reserve(200'000);
            for (std::size_t index = 0; index < 200'000; ++index)
                content_.push_back(static_cast<char>('a' + index % 26));
            std::ofstream{path(), std::ios::binary} << content_;
        }

        std::filesystem::path path() const
        {
            return directory_.path() / "file.bin";
        }

        TemporaryDirectory directory_{temporaryRoot, true};
        std::string content_{};
    };

    TEST_F(SharedInputFileStreamTests, ReadsTheFileInChunks)
    {
        SharedInputFileStream stream;
        stream.open(path());
        ASSERT_TRUE(stream.is_open());

        std::string read;
        std::array<char, 7'000> chunk{};
        while (stream.read(chunk.data(), chunk.size()) || stream.gcount() > 0)
            read.append(chunk.data(), static_cast<std::size_t>(stream.gcount()));

        EXPECT_EQ(read, content_);
        EXPECT_TRUE(stream.eof());
    }

    TEST_F(SharedInputFileStreamTests, SeeksAndTellsLikeAFileStream)
    {
        SharedInputFileStream stream;
        stream.open(path());

        stream.seekg(0, std::ios::end);
        EXPECT_EQ(static_cast<std::size_t>(stream.tellg()), content_.size());
        stream.seekg(0, std::ios::beg);
        std::array<char, 3> start{};
        ASSERT_TRUE(stream.read(start.data(), start.size()));
        EXPECT_EQ(std::string(start.data(), start.size()), "abc");
        EXPECT_EQ(stream.tellg(), std::streampos{3});
        stream.seekg(100'000);
        std::array<char, 2> middle{};
        ASSERT_TRUE(stream.read(middle.data(), middle.size()));
        EXPECT_EQ(std::string(middle.data(), middle.size()), content_.substr(100'000, 2));
        EXPECT_TRUE(stream.good());
    }

    TEST_F(SharedInputFileStreamTests, ReadingPastTheEndFailsWithWhatThereWas)
    {
        SharedInputFileStream stream;
        stream.open(path());
        stream.seekg(static_cast<std::streamoff>(content_.size() - 4));

        std::array<char, 10> tail{};
        stream.read(tail.data(), tail.size());

        EXPECT_EQ(stream.gcount(), 4);
        EXPECT_FALSE(stream.good());
        EXPECT_EQ(std::string(tail.data(), 4), content_.substr(content_.size() - 4));
    }

    TEST_F(SharedInputFileStreamTests, TheFileCanBeRenamedAndDeletedWhileItIsOpen)
    {
        SharedInputFileStream stream;
        stream.open(path());
        ASSERT_TRUE(stream.is_open());

        std::error_code renameError;
        std::filesystem::rename(path(), directory_.path() / "renamed.bin", renameError);
        EXPECT_FALSE(renameError) << renameError.message();
        std::error_code removeError;
        std::filesystem::remove(directory_.path() / "renamed.bin", removeError);
        EXPECT_FALSE(removeError) << removeError.message();

        std::array<char, 5> start{};
        ASSERT_TRUE(stream.read(start.data(), start.size()));
        EXPECT_EQ(std::string(start.data(), start.size()), "abcde");
        stream.close();
        EXPECT_FALSE(stream.is_open());
    }

    TEST_F(SharedInputFileStreamTests, AMissingFileFailsToOpen)
    {
        SharedInputFileStream stream;
        stream.open(directory_.path() / "missing.bin");

        EXPECT_FALSE(stream.is_open());
        EXPECT_TRUE(stream.fail());
    }

    TEST_F(SharedInputFileStreamTests, AFileReadToItsEndHasNoError)
    {
        SharedInputFileStream stream;
        stream.open(path());
        ASSERT_TRUE(stream.is_open());

        std::string read(content_.size() + 10u, '\0');
        stream.read(read.data(), static_cast<std::streamsize>(read.size()));

        EXPECT_EQ(static_cast<std::size_t>(stream.gcount()), content_.size());
        EXPECT_TRUE(stream.eof());
        EXPECT_FALSE(stream.lastError()) << stream.lastError().message();
    }

    TEST_F(SharedInputFileStreamTests, AMissingFileTellsTheSystemsError)
    {
        SharedInputFileStream stream;
        stream.open(directory_.path() / "missing.bin");

#ifdef _WIN32
        const auto windowsFileNotFound = 2;
        EXPECT_EQ(stream.lastError(), (std::error_code{windowsFileNotFound, std::system_category()}))
            << stream.lastError().message();
#else
        EXPECT_EQ(stream.lastError(), std::errc::no_such_file_or_directory) << stream.lastError().message();
#endif
    }

    TEST_F(SharedInputFileStreamTests, OpeningAgainForgetsTheLastError)
    {
        SharedInputFileStream stream;
        stream.open(directory_.path() / "missing.bin");
        ASSERT_TRUE(stream.lastError());

        stream.open(path());

        EXPECT_TRUE(stream.is_open());
        EXPECT_FALSE(stream.lastError()) << stream.lastError().message();
    }

    TEST_F(SharedInputFileStreamTests, TheErrorStaysAfterTheStreamIsClosed)
    {
        SharedInputFileStream stream;
        stream.open(directory_.path() / "missing.bin");

        stream.close();

        EXPECT_TRUE(stream.lastError());
    }

#ifdef _WIN32
    namespace
    {
        struct TestHandleCloser
        {
            void operator()(HANDLE handle) const
            {
                ::CloseHandle(handle);
            }
        };
        using TestHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, TestHandleCloser>;

        TestHandle openForTest(std::filesystem::path const& path, DWORD shareMode)
        {
            const auto handle = ::CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                shareMode,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            return TestHandle{handle == INVALID_HANDLE_VALUE ? nullptr : handle};
        }
    }

    TEST_F(SharedInputFileStreamTests, AFileAnotherProgramHoldsWithoutSharingTellsTheSharingViolation)
    {
        const auto holder = openForTest(path(), 0);
        ASSERT_TRUE(holder);

        SharedInputFileStream stream;
        stream.open(path());

        EXPECT_FALSE(stream.is_open());
        EXPECT_EQ(
            stream.lastError(), (std::error_code{static_cast<int>(ERROR_SHARING_VIOLATION), std::system_category()})
        ) << stream.lastError().message();
    }

    TEST_F(SharedInputFileStreamTests, AReadOfARangeAnotherProgramLockedTellsTheLockViolation)
    {
        const auto holder = openForTest(path(), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        ASSERT_TRUE(holder);
        OVERLAPPED overlapped{};
        ASSERT_TRUE(
            ::LockFileEx(
                holder.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped
            )
        );
        auto unlock = Nui::ScopeExit{[&holder]() noexcept
            {
                OVERLAPPED unlocked{};
                ::UnlockFileEx(holder.get(), 0, MAXDWORD, MAXDWORD, &unlocked);
            }};

        SharedInputFileStream stream;
        stream.open(path());
        ASSERT_TRUE(stream.is_open()) << stream.lastError().message();
        std::array<char, 16> start{};
        stream.read(start.data(), start.size());

        EXPECT_TRUE(stream.fail());
        EXPECT_EQ(stream.lastError(), (std::error_code{static_cast<int>(ERROR_LOCK_VIOLATION), std::system_category()}))
            << stream.lastError().message();
    }
#else
    TEST_F(SharedInputFileStreamTests, AFileItMayNotReadTellsPermissionDenied)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can read any file";
        std::filesystem::permissions(path(), std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[this]() noexcept
            {
                std::error_code errorCode{};
                std::filesystem::permissions(path(), std::filesystem::perms::owner_all, errorCode);
            }};

        SharedInputFileStream stream;
        stream.open(path());

        EXPECT_FALSE(stream.is_open());
        EXPECT_EQ(stream.lastError(), std::errc::permission_denied) << stream.lastError().message();
    }
#endif

#ifdef __linux__
    TEST_F(SharedInputFileStreamTests, AFailedReadTellsTheSystemsError)
    {
        // Linux opens a directory for reading, only reading it fails.
        SharedInputFileStream stream;
        stream.open(directory_.path());
        ASSERT_TRUE(stream.is_open()) << stream.lastError().message();
        std::array<char, 16> start{};
        stream.read(start.data(), start.size());

        EXPECT_TRUE(stream.fail());
        EXPECT_EQ(stream.lastError(), std::errc::is_a_directory) << stream.lastError().message();
    }
#endif
}
