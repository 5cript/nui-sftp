#pragma once

#include <persistence/state_core.hpp>

#include <fmt/format.h>

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>

namespace Persistence
{
    enum class SymlinkHandling
    {
        // Best on unix systems
        AsSymlink,
        // Sometimes the only option on windows, and best for compatibility with other tools.
        FollowSymlink,
        // Best for safety, but can cause problems if the user actually wanted to transfer a symlink (e.g. on windows).
        SkipSymlink
    };
    BOOST_DESCRIBE_ENUM(SymlinkHandling, AsSymlink, FollowSymlink, SkipSymlink)

    constexpr std::string_view defaultTempFileSuffix = ".filepart";

    /**
     * @brief The suffix a transfer writes a file under until it is complete.
     * @param configured The tempFileSuffix option; empty, missing or containing a path separator falls back to
     *        defaultTempFileSuffix.
     */
    inline std::string effectiveTempFileSuffix(std::optional<std::string> const& configured)
    {
        if (!configured || configured->empty() || configured->find_first_of("/\\") != std::string::npos)
            return std::string{defaultTempFileSuffix};
        return *configured;
    }

    /**
     * @brief The suffix a destination is moved aside to while a finished transfer replaces it on a server without
     *        posix-rename.
     * @param tempFileSuffix The effective temp file suffix, see effectiveTempFileSuffix.
     */
    inline std::string asideFileSuffix(std::string_view tempFileSuffix)
    {
        return fmt::format("{}.old", tempFileSuffix);
    }

    /**
     * @brief Whether a transfer gives the destination the mode of its source when inheritPermissions is not set.
     */
    inline constexpr bool defaultInheritPermissions = true;

    /**
     * @brief Whether a transfer continues a partial temp file of an earlier attempt when tryContinue is not set.
     */
    inline constexpr bool defaultTryContinue = true;

    /**
     * @brief Whether a download marks its file as downloaded from the internet when markAsDownloaded is not set.
     */
    inline constexpr bool defaultMarkAsDownloaded = true;

    /**
     * @brief Whether a canceled transfer deletes its partial file when doCleanup is not set.
     */
    inline constexpr bool defaultDoCleanup = true;

    struct CommonTransferOptions : public DefaultMissingMember
    {
        std::optional<std::string> tempFileSuffix{std::nullopt};
        std::optional<bool> mayOverwrite{std::nullopt};
        std::optional<bool> tryContinue{std::nullopt};
        std::optional<bool> inheritPermissions{std::nullopt};
        std::optional<std::filesystem::perms> customFilePermissions{std::nullopt};
        std::optional<std::filesystem::perms> customDirectoryPermissions{std::nullopt};
        std::optional<SymlinkHandling> symlinkHandling{std::nullopt};
        std::optional<bool> failFast{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        CommonTransferOptions,
        (),
        (tempFileSuffix,
            mayOverwrite,
            tryContinue,
            inheritPermissions,
            customFilePermissions,
            customDirectoryPermissions,
            symlinkHandling,
            failFast)
    )

    struct DownloadOptions : public CommonTransferOptions
    {
        std::optional<bool> reserveSpace{std::nullopt};
        std::optional<bool> doCleanup{std::nullopt};
        /**
         * @brief Mark downloaded files as coming from the internet (Mark of the Web on Windows, quarantine on macOS).
         *        Unset means on.
         */
        std::optional<bool> markAsDownloaded{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(DownloadOptions, (CommonTransferOptions), (reserveSpace, doCleanup, markAsDownloaded))

    struct UploadOptions : public CommonTransferOptions
    {
        /**
         * @brief Delete the partial file on the server when an upload is canceled. Unset means on.
         */
        std::optional<bool> doCleanup{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(UploadOptions, (CommonTransferOptions), (doCleanup))

    // Default number of queue operations worked on in parallel.
    inline constexpr int defaultSftpConcurrency = 4;

    // Default number of files a single bulk transfer may move at once. Draws from the
    // concurrency budget above, so it is clamped to it at runtime.
    inline constexpr int defaultBulkConcurrency = 2;

    struct SftpOptions
    {
        std::optional<DownloadOptions> downloadOptions{};
        std::optional<UploadOptions> uploadOptions{};
        std::optional<std::filesystem::path> defaultDirectory{std::nullopt};
        // How many parallel transfers are allowed?
        std::optional<int> concurrency{std::nullopt};
        // How many of those a single bulk transfer may occupy.
        std::optional<int> bulkConcurrency{std::nullopt};
        std::optional<std::chrono::seconds> operationTimeout{std::nullopt};
        // Whether ~/.config/user-dirs.dirs is read on the server to find the default places.
        std::optional<bool> readUserDirectories{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        SftpOptions,
        (),
        (downloadOptions,
            uploadOptions,
            defaultDirectory,
            concurrency,
            bulkConcurrency,
            operationTimeout,
            readUserDirectories)
    )
}