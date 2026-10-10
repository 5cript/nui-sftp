#pragma once

#include <backend/file_tracking/temp_dir_instancing.hpp>

#include <optional>
#include <string>

namespace FileTracking
{
    struct FileChange
    {
        FileAction action;
        std::string filename;
        std::string oldFilename{};
    };

    /**
     * @brief Whether @p filename is a temporary that an editor, a file manager or a download creates next to the
     *        real file: GNOME's .goutputstream, Finder's .DS_Store and AppleDouble files, AppKit's safe save and
     *        downloads still in progress.
     * @param downloadTempSuffix The suffix of downloads in progress, see Persistence::effectiveTempFileSuffix.
     */
    bool isTransientFile(std::string const& filename, std::string const& downloadTempSuffix);

    /**
     * @brief Turns a raw watcher event into the change to mirror to the remote side.
     * @param change The event as the watcher reports it.
     * @param isDirectory Whether the changed path is a directory.
     * @param downloadTempSuffix The suffix of downloads in progress, see Persistence::effectiveTempFileSuffix.
     * @return The change to mirror, nothing when the event is noise.
     */
    std::optional<FileChange>
    filterFileChange(FileChange change, bool isDirectory, std::string const& downloadTempSuffix);
}
