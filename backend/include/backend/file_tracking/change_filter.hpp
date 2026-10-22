#pragma once

#include <backend/file_tracking/file_action.hpp>

#include <filesystem>
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
     * @brief Whether a change of @p filename, moved from @p oldFilename when that is not empty, is noise nobody
     *        follows: it ends at a temporary and does not come from a real file. Cheap enough for the watcher's
     *        thread, which drops it before anything else looks at it.
     * @param downloadTempSuffix The suffix of downloads in progress, see Persistence::effectiveTempFileSuffix.
     */
    bool isTransientChange(
        std::string const& filename,
        std::string const& oldFilename,
        std::string const& downloadTempSuffix
    );

    /**
     * @brief Turns a raw watcher event into the change to mirror to the remote side.
     * @param change The event as the watcher reports it.
     * @param isDirectory Whether the changed path is a directory.
     * @param downloadTempSuffix The suffix of downloads in progress, see Persistence::effectiveTempFileSuffix.
     * @return The change to mirror, nothing when the event is noise.
     */
    std::optional<FileChange>
    filterFileChange(FileChange change, bool isDirectory, std::string const& downloadTempSuffix);

    /**
     * @brief Whether a change of @p path, moved from @p oldPath when that is not empty, touches @p item or anything
     *        below it. All paths are spelled alike (canonical).
     */
    bool isChangeOfItem(
        std::filesystem::path const& item,
        std::filesystem::path const& path,
        std::filesystem::path const& oldPath
    );

    /**
     * @brief Whether @p path is @p base or lies below it, compared lexically. An empty @p path is neither.
     */
    bool isAtOrBelow(std::filesystem::path const& base, std::filesystem::path const& path);
}
