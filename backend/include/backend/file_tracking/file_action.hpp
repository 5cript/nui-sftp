#pragma once

#include <utility/describe.hpp>

namespace FileTracking
{
    /**
     * @brief What happened to a watched path, mirrors the efsw action values and its missed file actions.
     */
    enum class FileAction
    {
        Added,
        Modified,
        Deleted,
        Moved,
        /**
         * @brief The watch lost the changes in a directory: its event queue overflowed, or the directory went while
         *        it was read. What is there has to be looked at anew. It names the directory, no entry in it.
         */
        Missed,
    };
    BOOST_DESCRIBE_ENUM(FileAction, Added, Modified, Deleted, Moved, Missed)
}
