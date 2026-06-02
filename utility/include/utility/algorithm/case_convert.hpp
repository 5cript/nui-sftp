#pragma once

#include <numeric>
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>

namespace Utility::Algorithm
{
    /**
     * @brief Converts the passed string to upper case by out paramter.
     *
     * @param input The string to convert.
     */
    inline void toUpperCaseInplace(std::string& input)
    {
        std::transform(input.begin(), input.end(), input.begin(), [](char c) {
            return std::toupper(c);
        });
    }

    /**
     * @brief Converts the passed string to upper case and returns it.
     *
     * @param input The string to convert.
     * @return std::string The string in upper case.
     */
    inline std::string toUpperCase(std::string const& input)
    {
        std::string result(input.size(), '\0');
        std::transform(input.begin(), input.end(), result.begin(), [](char c) {
            return std::toupper(c);
        });
        return result;
    }

    /**
     * @brief Converts the passed string to lower case by out paramter.
     *
     * @param input The string to convert.
     */
    inline void toLowerCaseInplace(std::string& input)
    {
        std::transform(input.begin(), input.end(), input.begin(), [](char c) {
            return std::tolower(c);
        });
    }

    /**
     * @brief Converts the passed string to lower case and returns it.
     *
     * @param input The string to convert.
     * @return std::string The string in lower case.
     */
    inline std::string toLowerCase(std::string const& input)
    {
        std::string result(input.size(), '\0');
        std::transform(input.begin(), input.end(), result.begin(), [](char c) {
            return std::tolower(c);
        });
        return result;
    }

    /**
     * @brief Case-insensitive lexicographic comparison of two strings.
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
            const auto leftChar = std::tolower(static_cast<unsigned char>(lhs[index]));
            const auto rightChar = std::tolower(static_cast<unsigned char>(rhs[index]));
            if (leftChar != rightChar)
                return leftChar < rightChar ? -1 : 1;
        }
        if (lhs.size() == rhs.size())
            return 0;
        return lhs.size() < rhs.size() ? -1 : 1;
    }
}