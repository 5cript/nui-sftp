#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace Paths
{
    /**
     * @brief U+FFFD, written in place of every byte or code unit that is not part of a valid sequence.
     */
    constexpr char32_t replacementCharacter = 0xFFFDu;

    /**
     * @brief A 16 bit code unit type: char16_t, and wchar_t on Windows.
     */
    template <typename CharT>
    concept Utf16CodeUnit = (std::same_as<CharT, char16_t> || std::same_as<CharT, wchar_t>) && sizeof(CharT) == 2;

    namespace Detail
    {
        /**
         * @brief Length of the well-formed UTF-8 sequence that starts at @p index of @p text, 0 when the bytes there
         *        are none (stray continuation byte, overlong form, surrogate, above U+10FFFF or truncated).
         */
        constexpr std::size_t utf8SequenceLength(std::string_view text, std::size_t index) noexcept
        {
            const auto lead = static_cast<unsigned char>(text[index]);
            if (lead < 0x80u)
                return 1u;

            const auto [length, initial] = [lead]() -> std::pair<std::size_t, std::uint32_t>
            {
                if ((lead & 0xE0u) == 0xC0u)
                    return {2u, lead & 0x1Fu};
                if ((lead & 0xF0u) == 0xE0u)
                    return {3u, lead & 0x0Fu};
                if ((lead & 0xF8u) == 0xF0u)
                    return {4u, lead & 0x07u};
                return {0u, 0u};
            }();
            if (length == 0u || index + length > text.size())
                return 0u;

            std::uint32_t codePoint = initial;
            for (std::size_t offset = 1; offset < length; ++offset)
            {
                const auto continuation = static_cast<unsigned char>(text[index + offset]);
                if ((continuation & 0xC0u) != 0x80u)
                    return 0u;
                codePoint = (codePoint << 6u) | (continuation & 0x3Fu);
            }
            constexpr std::array<std::uint32_t, 5> minimumForLength{0u, 0u, 0x80u, 0x800u, 0x10000u};
            if (codePoint < minimumForLength[length] || codePoint > 0x10FFFFu ||
                (codePoint >= 0xD800u && codePoint <= 0xDFFFu))
                return 0u;
            return length;
        }

        /**
         * @brief The code point of the valid sequence of @p length bytes at @p index of @p text, see
         *        @ref utf8SequenceLength.
         */
        constexpr char32_t utf8CodePointAt(std::string_view text, std::size_t index, std::size_t length) noexcept
        {
            constexpr std::array<std::uint32_t, 5> leadMask{0u, 0x7Fu, 0x1Fu, 0x0Fu, 0x07u};
            std::uint32_t codePoint = static_cast<unsigned char>(text[index]) & leadMask[length];
            for (std::size_t offset = 1; offset < length; ++offset)
                codePoint = (codePoint << 6u) | (static_cast<unsigned char>(text[index + offset]) & 0x3Fu);
            return static_cast<char32_t>(codePoint);
        }

        /**
         * @brief True when @p codePoint is a Unicode scalar value: at most U+10FFFF and no surrogate.
         */
        constexpr bool isScalarValue(char32_t codePoint) noexcept
        {
            return codePoint <= 0x10FFFFu && (codePoint < 0xD800u || codePoint > 0xDFFFu);
        }

        /**
         * @brief Appends @p codePoint to @p text as UTF-8, U+FFFD when it is no scalar value.
         */
        constexpr void appendUtf8(std::string& text, char32_t codePoint)
        {
            if (!isScalarValue(codePoint))
                codePoint = replacementCharacter;
            const auto byte = [](std::uint32_t value)
            {
                return static_cast<char>(static_cast<unsigned char>(value));
            };
            if (codePoint < 0x80u)
            {
                text.push_back(byte(codePoint));
            }
            else if (codePoint < 0x800u)
            {
                text.push_back(byte(0xC0u | (codePoint >> 6u)));
                text.push_back(byte(0x80u | (codePoint & 0x3Fu)));
            }
            else if (codePoint < 0x10000u)
            {
                text.push_back(byte(0xE0u | (codePoint >> 12u)));
                text.push_back(byte(0x80u | ((codePoint >> 6u) & 0x3Fu)));
                text.push_back(byte(0x80u | (codePoint & 0x3Fu)));
            }
            else
            {
                text.push_back(byte(0xF0u | (codePoint >> 18u)));
                text.push_back(byte(0x80u | ((codePoint >> 12u) & 0x3Fu)));
                text.push_back(byte(0x80u | ((codePoint >> 6u) & 0x3Fu)));
                text.push_back(byte(0x80u | (codePoint & 0x3Fu)));
            }
        }

        /**
         * @brief Calls @p onCodePoint with every code point of @p text, U+FFFD for each byte that is not part of a
         *        valid sequence.
         */
        template <typename FunctionT>
        constexpr void forEachUtf8CodePoint(std::string_view text, FunctionT&& onCodePoint)
        {
            std::size_t index = 0;
            while (index < text.size())
            {
                const auto length = utf8SequenceLength(text, index);
                if (length == 0u)
                {
                    onCodePoint(replacementCharacter);
                    ++index;
                    continue;
                }
                onCodePoint(utf8CodePointAt(text, index, length));
                index += length;
            }
        }

        /**
         * @brief Calls @p onCodePoint with every code point of the UTF-16 @p text, U+FFFD for each unpaired
         *        surrogate.
         */
        template <Utf16CodeUnit CharT, typename FunctionT>
        constexpr void forEachUtf16CodePoint(std::basic_string_view<CharT> text, FunctionT&& onCodePoint)
        {
            std::size_t index = 0;
            while (index < text.size())
            {
                const auto unit = static_cast<char32_t>(static_cast<std::uint16_t>(text[index]));
                const bool isHigh = unit >= 0xD800u && unit <= 0xDBFFu;
                const auto next = index + 1 < text.size()
                    ? static_cast<char32_t>(static_cast<std::uint16_t>(text[index + 1]))
                    : char32_t{0};
                if (isHigh && next >= 0xDC00u && next <= 0xDFFFu)
                {
                    onCodePoint(0x10000u + ((unit - 0xD800u) << 10u) + (next - 0xDC00u));
                    index += 2;
                    continue;
                }
                onCodePoint(unit >= 0xD800u && unit <= 0xDFFFu ? replacementCharacter : unit);
                ++index;
            }
        }
    }

    /**
     * @brief True when @p text is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
     */
    constexpr bool isValidUtf8(std::string_view text) noexcept
    {
        std::size_t index = 0;
        while (index < text.size())
        {
            const auto length = Detail::utf8SequenceLength(text, index);
            if (length == 0u)
                return false;
            index += length;
        }
        return true;
    }

    /**
     * @brief @p text with every byte that is not part of a valid UTF-8 sequence replaced by U+FFFD, so the result
     *        is always valid UTF-8. Valid text comes back unchanged.
     */
    constexpr std::string sanitizedUtf8(std::string_view text)
    {
        if (isValidUtf8(text))
            return std::string{text};
        std::string result;
        result.reserve(text.size() + 8u);
        Detail::forEachUtf8CodePoint(
            text,
            [&result](char32_t codePoint)
            {
                Detail::appendUtf8(result, codePoint);
            }
        );
        return result;
    }

    /**
     * @brief The longest start of @p text with at most @p maximumBytes bytes that does not end inside a valid
     *        UTF-8 sequence. A byte outside any valid sequence counts as a sequence of its own.
     */
    constexpr std::string_view truncatedUtf8(std::string_view text, std::size_t maximumBytes) noexcept
    {
        if (text.size() <= maximumBytes)
            return text;
        std::size_t end = 0;
        while (end < text.size())
        {
            const auto length = Detail::utf8SequenceLength(text, end);
            const auto step = length == 0u ? 1u : length;
            if (end + step > maximumBytes)
                break;
            end += step;
        }
        return text.substr(0, end);
    }

    /**
     * @brief The number of UTF-16 code units of the valid UTF-8 @p text, the unit Windows limits names in.
     */
    constexpr std::size_t utf16Length(std::string_view text) noexcept
    {
        std::size_t units = 0;
        std::size_t index = 0;
        while (index < text.size())
        {
            const auto length = Detail::utf8SequenceLength(text, index);
            units += length == 4u ? 2u : 1u;
            index += length == 0u ? 1u : length;
        }
        return units;
    }

    /**
     * @brief True when the UTF-16 @p text has no unpaired surrogate.
     */
    template <Utf16CodeUnit CharT>
    constexpr bool isValidUtf16(std::basic_string_view<CharT> text) noexcept
    {
        bool valid = true;
        std::size_t index = 0;
        while (index < text.size() && valid)
        {
            const auto unit = static_cast<std::uint16_t>(text[index]);
            if (unit >= 0xD800u && unit <= 0xDBFFu && index + 1 < text.size())
            {
                const auto next = static_cast<std::uint16_t>(text[index + 1]);
                if (next >= 0xDC00u && next <= 0xDFFFu)
                {
                    index += 2;
                    continue;
                }
            }
            valid = unit < 0xD800u || unit > 0xDFFFu;
            ++index;
        }
        return valid;
    }

    /**
     * @brief The UTF-8 @p text as UTF-16, every byte that is not part of a valid sequence becomes U+FFFD.
     */
    template <Utf16CodeUnit CharT = char16_t>
    constexpr std::basic_string<CharT> utf8ToUtf16(std::string_view text)
    {
        std::basic_string<CharT> result;
        result.reserve(text.size());
        Detail::forEachUtf8CodePoint(
            text,
            [&result](char32_t codePoint)
            {
                if (codePoint < 0x10000u)
                {
                    result.push_back(static_cast<CharT>(codePoint));
                    return;
                }
                const auto offset = codePoint - 0x10000u;
                result.push_back(static_cast<CharT>(0xD800u + (offset >> 10u)));
                result.push_back(static_cast<CharT>(0xDC00u + (offset & 0x3FFu)));
            }
        );
        return result;
    }

    /**
     * @brief The UTF-16 @p text as UTF-8, every unpaired surrogate becomes U+FFFD.
     */
    template <Utf16CodeUnit CharT>
    constexpr std::string utf16ToUtf8(std::basic_string_view<CharT> text)
    {
        std::string result;
        result.reserve(text.size());
        Detail::forEachUtf16CodePoint(
            text,
            [&result](char32_t codePoint)
            {
                Detail::appendUtf8(result, codePoint);
            }
        );
        return result;
    }

#ifdef _WIN32
    /**
     * @brief UTF-8 to the UTF-16 text of the wide Windows API, see @ref utf8ToUtf16.
     */
    inline std::wstring utf8ToWide(std::string_view text)
    {
        return utf8ToUtf16<wchar_t>(text);
    }

    /**
     * @brief Wide Windows API text to UTF-8, see @ref utf16ToUtf8.
     */
    inline std::string wideToUtf8(std::wstring_view text)
    {
        return utf16ToUtf8(text);
    }
#endif
}
