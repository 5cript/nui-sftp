#pragma once

#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/directory_entry.hpp>
#include <utility/user_directories.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RemoteIo
{
    /**
     * @brief Renames without replacing @p destination, fails with AlreadyExists when it exists. Must be called on the
     *        processing thread of the file system's strand.
     *
     * A regular file moves with LinkIntoPlace where Capabilities::hardLinks is set, atomically where the link
     * succeeds. Otherwise a file system whose rename refuses an existing destination is left to refuse it, and on
     * any other the destination is checked before the rename, which leaves a window. A destination that cannot be
     * checked is never renamed over.
     *
     * @param sourceType The type of @p source when the caller knows it, saves an Lstat.
     */
    std::expected<void, RemoteError> renameNoReplaceInStrand(
        IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        std::optional<SharedData::FileType> sourceType = std::nullopt
    );

    /**
     * @brief Renames over an existing @p destination. Must be called on the processing thread of the file system's
     *        strand.
     *
     * One rename where the file system's rename replaces. Otherwise the destination is moved aside to
     * @p asideSuffix first and moved back when the rename fails; if moving it back fails too, the error names where
     * it is kept. A leftover aside is removed when the destination exists, and put back when it does not and the
     * rename fails.
     */
    std::expected<void, RemoteError> renameReplaceInStrand(
        IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        std::string_view asideSuffix
    );

    /**
     * @brief Moves a finished transfer from @p source to @p destination with @ref renameReplaceInStrand when
     *        @p replace is set, otherwise with @ref renameNoReplaceInStrand.
     */
    std::expected<void, RemoteError> moveIntoPlaceInStrand(
        IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        bool replace,
        std::string_view asideSuffix,
        std::optional<SharedData::FileType> sourceType = std::nullopt
    );

    /**
     * @brief Reads a file into a string through OpenFile, stopping after @p maximumSize bytes: ReadFile for a protocol
     *        whose opens never fail with Busy. Must be called on the processing thread of the file system's strand.
     */
    std::expected<std::string, RemoteError>
    readSmallFileInStrand(IRemoteFileSystem& fileSystem, Paths::RemotePath const& path, std::size_t maximumSize);

    /**
     * @brief Reads the XDG user directories from <home>/.config/user-dirs.dirs. A missing file, or a file system
     *        without Capabilities::userDirectories, yields no directories instead of an error. Must be called on the
     *        processing thread of the file system's strand.
     */
    std::expected<std::vector<Utility::UserDirectory>, RemoteError>
    userDirectoriesInStrand(IRemoteFileSystem& fileSystem, Paths::RemotePath const& home);
}
