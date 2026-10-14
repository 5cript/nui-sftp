#pragma once

#include <paths/utf8.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <iterator>
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
        /**
         * @brief A character the local platform cannot store in a name, like '\\' or ':' on Windows.
         */
        ReservedCharacter,
        /**
         * @brief A Windows device name like CON or LPT1, with or without an extension.
         */
        ReservedDeviceName,
        /**
         * @brief Windows drops a trailing dot or space, the name would refer to another file.
         */
        TrailingDotOrSpace,
        /**
         * @brief The path leaves the directory it is meant to stay in.
         */
        EscapesRoot,
        /**
         * @brief A Windows device or namespace path like \\.\PhysicalDrive0 or \\?\Volume{...}, no file path.
         */
        DevicePath,
        /**
         * @brief A name longer than the 255 UTF-16 code units Windows allows.
         */
        NameTooLong,
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
            case PathRejection::DevicePath:
                return "is a device path";
            case PathRejection::NameTooLong:
                return "has a name that is too long";
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

        /**
         * @brief The input in quotes followed by the reason. Control bytes and bytes that are not part of a valid
         *        UTF-8 sequence are written as \\xNN, so the text is always valid UTF-8.
         */
        std::string toString() const
        {
            std::string printable;
            printable.reserve(input.size());
            std::size_t index = 0;
            while (index < input.size())
            {
                const auto byte = static_cast<unsigned char>(input[index]);
                const auto length = Detail::utf8SequenceLength(input, index);
                if (length == 0u || byte < 0x20u || byte == 0x7Fu)
                {
                    fmt::format_to(std::back_inserter(printable), "\\x{:02X}", byte);
                    ++index;
                    continue;
                }
                printable.append(input, index, length);
                index += length;
            }
            return fmt::format("'{}' {}", printable, Paths::toString(reason));
        }
    };
}
