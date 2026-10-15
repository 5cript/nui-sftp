#pragma once

#include <paths/path_error.hpp>

namespace FileExplorer
{
    /**
     * @brief The language key below "fileExplorer.nameRejection" that explains why a new name was refused for
     *        @p rejection, "Other" for a rejection no single name gets.
     */
    constexpr char const* nameRejectionKey(Paths::PathRejection rejection)
    {
        switch (rejection)
        {
            case Paths::PathRejection::Empty:
                return "Empty";
            case Paths::PathRejection::DotOrDotDot:
                return "DotOrDotDot";
            case Paths::PathRejection::ContainsSeparator:
                return "ContainsSeparator";
            case Paths::PathRejection::ContainsNul:
                return "ContainsNul";
            case Paths::PathRejection::InvalidUtf8:
                return "InvalidUtf8";
            case Paths::PathRejection::ReservedCharacter:
                return "ReservedCharacter";
            case Paths::PathRejection::ReservedDeviceName:
                return "ReservedDeviceName";
            case Paths::PathRejection::TrailingDotOrSpace:
                return "TrailingDotOrSpace";
            case Paths::PathRejection::NameTooLong:
                return "NameTooLong";
            case Paths::PathRejection::NotAbsolute:
            case Paths::PathRejection::EscapesRoot:
            case Paths::PathRejection::DevicePath:
            case Paths::PathRejection::UnknownParent:
                return "Other";
        }
        return "Other";
    }
}
