#pragma once

#include <shared_data/file_operations/operation_error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace SharedData::Test
{
    TEST(OperationErrorTests, EveryWrapperErrorSurvivesJson)
    {
        using SecureShell::WrapperErrors;
        for (int value = static_cast<int>(WrapperErrors::None); value <= static_cast<int>(WrapperErrors::InvalidPath);
             ++value)
        {
            const SecureShell::SftpError error{.message = "refused", .wrapperError = static_cast<WrapperErrors>(value)};
            nlohmann::json json{};
            ASSERT_NO_THROW(json = error) << "wrapper error " << value;
            EXPECT_EQ(json.get<SecureShell::SftpError>().wrapperError, error.wrapperError) << "wrapper error " << value;
        }
    }

    TEST(OperationErrorTests, ExtraInfoReachesTheOtherSide)
    {
        const OperationError error{
            .type = OperationErrorType::FileExists,
            .sftpError = SecureShell::SftpError{.message = "exists", .sftpError = 11},
            .extraInfo = "no free name is left",
        };

        const nlohmann::json json = error;
        const auto read = json.get<OperationError>();

        EXPECT_EQ(read.extraInfo, "no free name is left");
        EXPECT_EQ(read.toString(), error.toString());
    }

    TEST(OperationErrorTests, ErrorWithoutExtraInfoStaysWithout)
    {
        const nlohmann::json json = OperationError{.type = OperationErrorType::UnknownError};

        EXPECT_FALSE(json.get<OperationError>().extraInfo.has_value());
    }

    TEST(OperationErrorTests, StatusCodesOfLaterSftpVersionsHaveNames)
    {
        using SecureShell::SftpError;
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(9), "Invalid handle");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(11), "File already exists");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(14), "No space on file system");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(15), "Quota exceeded");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(18), "Directory not empty");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(31), "No matching byte range lock");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(32), "Unknown error code: 32");
        EXPECT_EQ(SftpError::sftpErrorToComprehensible(-1), "Unknown error code: -1");
    }
}
