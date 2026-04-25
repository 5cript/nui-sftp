#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>

namespace NuiSftpPage::Utf8
{
    /**
     * @brief Encode a single Unicode codepoint as a UTF-8 byte sequence.
     *
     * Source files only need to spell numeric codepoints (e.g. 0x2014), so
     * the UTF-8 bytes never appear in the C++ source -- editor encoding,
     * compiler execution charset, or build-system text-mode handling can
     * never corrupt them.
     *
     * @param codepoint Unicode scalar (0..0x10FFFF). Out-of-range yields "".
     * @return UTF-8 encoded std::string.
     */
    inline std::string cp(char32_t codepoint)
    {
        std::string out;
        if (codepoint < 0x80u)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint < 0x800u)
        {
            out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        }
        else if (codepoint < 0x10000u)
        {
            out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        }
        else if (codepoint < 0x110000u)
        {
            out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        }
        return out;
    }

    /**
     * @brief Encode a sequence of codepoints as one UTF-8 string.
     *
     * Convenience wrapper for runs of glyphs (e.g. arrows, separators,
     * decorative bullets) that you want to assemble in one expression.
     */
    inline std::string cps(std::initializer_list<char32_t> codepoints)
    {
        std::string out;
        out.reserve(codepoints.size() * 4u);
        for (auto codepoint : codepoints)
            out += cp(codepoint);
        return out;
    }
}
