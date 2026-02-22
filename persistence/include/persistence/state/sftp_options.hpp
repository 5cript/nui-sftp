#pragma once

#include <persistence/state_core.hpp>

#include <string>
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
    };
    BOOST_DESCRIBE_STRUCT(DownloadOptions, (CommonTransferOptions), (reserveSpace, doCleanup))

    struct UploadOptions : public CommonTransferOptions
    {};
    BOOST_DESCRIBE_STRUCT(UploadOptions, (CommonTransferOptions), ())

    struct SftpOptions
    {
        std::optional<DownloadOptions> downloadOptions{};
        std::optional<UploadOptions> uploadOptions{};
        std::optional<std::filesystem::path> defaultDirectory{std::nullopt};
        std::optional<int> concurrency{std::nullopt}; // How many parallel transfers are allowed?
        std::optional<std::chrono::seconds> operationTimeout{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        SftpOptions,
        (),
        (downloadOptions, uploadOptions, defaultDirectory, concurrency, operationTimeout)
    )
}