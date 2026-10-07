#include <utility/utf8.hpp>

#include <cstddef>

namespace Utility::Utf8
{
    namespace
    {
        constexpr char32_t replacementCharacter = 0xFFFDu;

        bool isContinuationByte(unsigned char byte)
        {
            return (byte & 0xC0u) == 0x80u;
        }

        bool isScalarValue(char32_t codePoint)
        {
            return codePoint <= 0x10FFFFu && (codePoint < 0xD800u || codePoint > 0xDFFFu);
        }
    }

    std::u32string decode(std::string_view utf8)
    {
        std::u32string result;
        result.reserve(utf8.size());

        std::size_t position = 0;
        while (position < utf8.size())
        {
            const auto leadByte = static_cast<unsigned char>(utf8[position]);
            if (leadByte < 0x80u)
            {
                result.push_back(leadByte);
                ++position;
                continue;
            }

            std::size_t length = 0;
            char32_t codePoint = 0;
            char32_t minimum = 0;
            if ((leadByte & 0xE0u) == 0xC0u)
            {
                length = 2;
                codePoint = leadByte & 0x1Fu;
                minimum = 0x80u;
            }
            else if ((leadByte & 0xF0u) == 0xE0u)
            {
                length = 3;
                codePoint = leadByte & 0x0Fu;
                minimum = 0x800u;
            }
            else if ((leadByte & 0xF8u) == 0xF0u)
            {
                length = 4;
                codePoint = leadByte & 0x07u;
                minimum = 0x10000u;
            }
            else
            {
                result.push_back(replacementCharacter);
                ++position;
                continue;
            }

            std::size_t consumed = 1;
            while (
                consumed < length && position + consumed < utf8.size() &&
                isContinuationByte(static_cast<unsigned char>(utf8[position + consumed]))
            )
            {
                codePoint = (codePoint << 6u) | (static_cast<unsigned char>(utf8[position + consumed]) & 0x3Fu);
                ++consumed;
            }

            if (consumed != length || codePoint < minimum || !isScalarValue(codePoint))
                result.push_back(replacementCharacter);
            else
                result.push_back(codePoint);
            position += consumed;
        }
        return result;
    }

    std::string encode(std::u32string_view codePoints)
    {
        std::string result;
        result.reserve(codePoints.size());

        for (auto codePoint : codePoints)
        {
            if (!isScalarValue(codePoint))
                codePoint = replacementCharacter;

            if (codePoint < 0x80u)
            {
                result.push_back(static_cast<char>(codePoint));
            }
            else if (codePoint < 0x800u)
            {
                result.push_back(static_cast<char>(0xC0u | (codePoint >> 6u)));
                result.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            }
            else if (codePoint < 0x10000u)
            {
                result.push_back(static_cast<char>(0xE0u | (codePoint >> 12u)));
                result.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
                result.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            }
            else
            {
                result.push_back(static_cast<char>(0xF0u | (codePoint >> 18u)));
                result.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu)));
                result.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
                result.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            }
        }
        return result;
    }
}
