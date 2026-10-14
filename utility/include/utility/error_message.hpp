#pragma once

#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

namespace Utility
{
    /**
     * @brief The text of @p error as valid UTF-8. On Windows a system error is read with FormatMessageW, which the
     *        narrow message() would give in the ANSI code page; any other text has every byte that is not UTF-8
     *        replaced by U+FFFD.
     */
    std::string errorMessage(std::error_code const& error);

    /**
     * @brief The text of a filesystem error as valid UTF-8: the message of its code and the paths it names.
     */
    std::string errorMessage(std::filesystem::filesystem_error const& error);

    /**
     * @brief The text of any exception as valid UTF-8: a filesystem error as above, any other what() with every byte
     *        that is not UTF-8 replaced by U+FFFD.
     */
    std::string errorMessage(std::exception const& error);
}
