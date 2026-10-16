#pragma once

#include <utility/error_message.hpp>
#include <paths/native.hpp>
#include <paths/utf8.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Utility::Test
{
    TEST(ErrorMessageTests, GenericErrorsKeepTheirMessage)
    {
        const auto error = std::make_error_code(std::errc::no_such_file_or_directory);
        EXPECT_EQ(errorMessage(error), Paths::sanitizedUtf8(error.message()));
        EXPECT_TRUE(Paths::isValidUtf8(errorMessage(error)));
    }

    TEST(ErrorMessageTests, SystemErrorsAreUtf8)
    {
        const std::error_code error{5, std::system_category()};
        const auto message = errorMessage(error);
        EXPECT_FALSE(message.empty());
        EXPECT_TRUE(Paths::isValidUtf8(message));
    }

    TEST(ErrorMessageTests, FilesystemErrorsNameTheirPathsAsUtf8)
    {
        const auto code = std::make_error_code(std::errc::permission_denied);
        const std::filesystem::filesystem_error error{
            "remove", Paths::nativeFromUtf8("/x/Grüße"), Paths::nativeFromUtf8("/y/東京"), code
        };
        EXPECT_EQ(errorMessage(error), fmt::format("{}: '/x/Grüße', '/y/東京'", errorMessage(code)));

        const std::filesystem::filesystem_error single{"remove", Paths::nativeFromUtf8("/x/a"), code};
        EXPECT_EQ(errorMessage(single), fmt::format("{}: '/x/a'", errorMessage(code)));

        const std::filesystem::filesystem_error none{"remove", code};
        EXPECT_EQ(errorMessage(none), errorMessage(code));
    }

    TEST(ErrorMessageTests, ExceptionsAreUtf8)
    {
        const std::runtime_error plain{"bad \xFF"};
        EXPECT_EQ(errorMessage(plain), "bad \xEF\xBF\xBD");

        const auto code = std::make_error_code(std::errc::permission_denied);
        const std::filesystem::filesystem_error filesystemError{"remove", Paths::nativeFromUtf8("/x/ü"), code};
        std::exception const& asException = filesystemError;
        EXPECT_EQ(errorMessage(asException), errorMessage(filesystemError));
    }

#ifndef _WIN32
    TEST(ErrorMessageTests, FilesystemErrorsReplaceBytesThatAreNotUtf8)
    {
        const std::filesystem::filesystem_error error{
            "remove", std::filesystem::path{std::string{"/x/Fran\xE7ois"}}, std::make_error_code(std::errc::io_error)
        };
        const auto message = errorMessage(error);
        EXPECT_TRUE(Paths::isValidUtf8(message));
        EXPECT_NE(message.find("/x/Fran\xEF\xBF\xBDois"), std::string::npos);
    }
#endif
}
