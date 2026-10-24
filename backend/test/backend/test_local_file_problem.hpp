#pragma once

#include <backend/sftp/local_file_problem.hpp>

#include <gtest/gtest.h>

#include <cerrno>
#include <ios>
#include <system_error>

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
}
