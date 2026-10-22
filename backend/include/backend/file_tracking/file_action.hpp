#pragma once

#include <utility/describe.hpp>

namespace FileTracking
{
    /**
     * @brief What happened to a watched path, mirrors the efsw action values.
     */
    enum class FileAction
    {
        Added,
        Modified,
        Deleted,
        Moved,
    };
    BOOST_DESCRIBE_ENUM(FileAction, Added, Modified, Deleted, Moved)
}
