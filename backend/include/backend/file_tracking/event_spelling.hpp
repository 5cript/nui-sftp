#pragma once

#include <string>

namespace FileTracking
{
    /**
     * @brief @p name spelled like a DirectoryEvent spells it: precomposed (NFC) on macOS, where efsw precomposes every
     *        name while the file system keeps the spelling a name was created with (Finder's is decomposed) and finds
     *        it under either. Unchanged elsewhere.
     */
    std::string eventSpelling(std::string const& name);
}
