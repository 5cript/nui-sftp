#pragma once

#include <numeric>
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>

namespace Utility::Algorithm
{
    /**
     * @brief @p character in lower case when it is an ASCII letter, unchanged otherwise. Bytes of a multibyte UTF-8
     *        sequence are never touched and the locale plays no part.
     */
    constexpr char asciiToLower(char character) noexcept
    {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    /**
     * @brief @p character in upper case when it is an ASCII letter, unchanged otherwise, see @ref asciiToLower.
     */
    constexpr char asciiToUpper(char character) noexcept
    {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character;
    }

    /**
     * @brief True for the ASCII upper case letters.
     */
    constexpr bool isAsciiUpper(char character) noexcept
    {
        return character >= 'A' && character <= 'Z';
    }

    /**
     * @brief Converts the ASCII letters of the passed string to upper case by out paramter.
     *
     * @param input The string to convert.
     */
    inline void toUpperCaseInplace(std::string& input)
    {
        std::ranges::transform(input, input.begin(), asciiToUpper);
    }

    /**
     * @brief Converts the ASCII letters of the passed string to upper case and returns it.
     *
     * @param input The string to convert.
     * @return std::string The string in upper case.
     */
    inline std::string toUpperCase(std::string const& input)
    {
        std::string result(input.size(), '\0');
        std::ranges::transform(input, result.begin(), asciiToUpper);
        return result;
    }

    /**
     * @brief Converts the ASCII letters of the passed string to lower case by out paramter.
     *
     * @param input The string to convert.
     */
    inline void toLowerCaseInplace(std::string& input)
    {
        std::ranges::transform(input, input.begin(), asciiToLower);
    }

    /**
     * @brief Converts the ASCII letters of the passed string to lower case and returns it.
     *
     * @param input The string to convert.
     * @return std::string The string in lower case.
     */
    inline std::string toLowerCase(std::string const& input)
    {
        std::string result(input.size(), '\0');
        std::ranges::transform(input, result.begin(), asciiToLower);
        return result;
    }

    /**
     * @brief Lexicographic comparison of two strings, ASCII letters compared without case.
     *
     * @param lhs The left-hand string.
     * @param rhs The right-hand string.
     * @return Negative if lhs orders before rhs, zero if equal, positive if lhs orders after
     *         rhs, all under case-insensitive ordering.
     */
    inline int caseInsensitiveCompare(std::string_view lhs, std::string_view rhs)
    {
        const auto commonLength = std::min(lhs.size(), rhs.size());
        for (std::size_t index = 0; index < commonLength; ++index)
        {
            const auto leftChar = static_cast<unsigned char>(asciiToLower(lhs[index]));
            const auto rightChar = static_cast<unsigned char>(asciiToLower(rhs[index]));
            if (leftChar != rightChar)
                return leftChar < rightChar ? -1 : 1;
        }
        if (lhs.size() == rhs.size())
            return 0;
        return lhs.size() < rhs.size() ? -1 : 1;
    }
}