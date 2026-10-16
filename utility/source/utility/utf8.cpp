#include <utility/utf8.hpp>

#include <paths/utf8.hpp>

namespace Utility::Utf8
{
    std::u32string decode(std::string_view utf8)
    {
        std::u32string result;
        result.reserve(utf8.size());
        Paths::Detail::forEachUtf8CodePoint(
            utf8,
            [&result](char32_t codePoint)
            {
                result.push_back(codePoint);
            }
        );
        return result;
    }

    std::string encode(std::u32string_view codePoints)
    {
        std::string result;
        result.reserve(codePoints.size());
        for (const auto codePoint : codePoints)
            Paths::Detail::appendUtf8(result, codePoint);
        return result;
    }
}
