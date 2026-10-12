#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Paths
{
    /**
     * @brief Why a string was refused as a path or path segment.
     */
    enum class PathRejection : std::uint8_t
    {
        Empty,
        DotOrDotDot,
        ContainsSeparator,
        ContainsNul,
        InvalidUtf8,
        NotAbsolute,
        /// A character the local platform cannot store in a name, like '\\' or ':' on Windows.
        ReservedCharacter,
        /// A Windows device name like CON or LPT1, with or without an extension.
        ReservedDeviceName,
        /// Windows drops a trailing dot or space, the name would refer to another file.
        TrailingDotOrSpace,
        /// The path leaves the directory it is meant to stay in.
        EscapesRoot,
    };

    inline std::string_view toString(PathRejection rejection)
    {
        switch (rejection)
        {
            case PathRejection::Empty:
                return "is empty";
            case PathRejection::DotOrDotDot:
                return "is '.' or '..'";
            case PathRejection::ContainsSeparator:
                return "contains a path separator";
            case PathRejection::ContainsNul:
                return "contains a NUL character";
            case PathRejection::InvalidUtf8:
                return "is not valid UTF-8";
            case PathRejection::NotAbsolute:
                return "is not absolute";
            case PathRejection::ReservedCharacter:
                return "contains a character the local system does not allow";
            case PathRejection::ReservedDeviceName:
                return "is a reserved device name";
            case PathRejection::TrailingDotOrSpace:
                return "ends with a dot or space";
            case PathRejection::EscapesRoot:
                return "leaves its root directory";
        }
        return "is invalid";
    }

    /**
     * @brief A refused path, with the input made printable.
     */
    struct PathError
    {
        PathRejection reason{PathRejection::Empty};
        std::string input{};

        std::string toString() const
        {
            std::string printable;
            printable.reserve(input.size());
            for (const auto character : input)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20u || byte == 0x7Fu)
                {
                    constexpr std::string_view digits = "0123456789ABCDEF";
                    printable += "\\x";
                    printable.push_back(digits[byte >> 4u]);
                    printable.push_back(digits[byte & 0x0Fu]);
                }
                else
                    printable.push_back(character);
            }
            return "'" + printable + "' " + std::string{Paths::toString(reason)};
        }
    };
}
