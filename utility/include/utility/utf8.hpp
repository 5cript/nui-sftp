#pragma once

#include <string>
#include <string_view>

namespace Utility::Utf8
{
    /**
     * @brief Decodes UTF-8 into code points. Every byte that is not part of a valid sequence (malformed, truncated,
     * overlong, surrogate) becomes one U+FFFD, so arbitrary bytes never fail to decode.
     *
     * @param utf8 The text to decode.
     */
    std::u32string decode(std::string_view utf8);

    /**
     * @brief Encodes code points as UTF-8. Code points that are not valid scalar values are written as U+FFFD.
     *
     * @param codePoints The code points to encode.
     */
    std::string encode(std::u32string_view codePoints);
}
