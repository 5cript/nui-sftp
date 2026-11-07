#pragma once

#include <utility/algorithm/case_convert.hpp>

#include <algorithm>
#include <string_view>

namespace FileTransferProtocol::Detail
{
    /**
     * @brief @p text without the spaces around it.
     */
    inline std::string_view trimmedSpaces(std::string_view text)
    {
        const auto start = text.find_first_not_of(' ');
        if (start == std::string_view::npos)
            return {};
        return text.substr(start, text.find_last_not_of(' ') - start + 1u);
    }

    /**
     * @brief @p left and @p right are equal, ASCII letters compared in any case.
     */
    inline bool equalIgnoringCase(std::string_view left, std::string_view right)
    {
        return Utility::Algorithm::caseInsensitiveCompare(left, right) == 0;
    }

    /**
     * @brief @p left and @p right are the same character, ASCII letters compared in any case.
     */
    inline bool sameCharacterIgnoringCase(char left, char right)
    {
        return Utility::Algorithm::asciiToLower(left) == Utility::Algorithm::asciiToLower(right);
    }

    /**
     * @brief @p text holds @p phrase, ASCII letters compared in any case.
     */
    inline bool containsIgnoringCase(std::string_view text, std::string_view phrase)
    {
        const auto found = std::ranges::search(text, phrase, sameCharacterIgnoringCase);
        return !found.empty();
    }

    /**
     * @brief @p text ends with @p phrase, ASCII letters compared in any case.
     */
    inline bool endsWithIgnoringCase(std::string_view text, std::string_view phrase)
    {
        return text.size() >= phrase.size() && equalIgnoringCase(text.substr(text.size() - phrase.size()), phrase);
    }
}
