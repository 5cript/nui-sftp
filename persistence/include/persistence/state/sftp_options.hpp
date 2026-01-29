#pragma once

#include <persistence/state_core.hpp>

#include <string>
#include <optional>
#include <filesystem>

namespace Persistence
{
    struct CommonTransferOptions : public DefaultMissingMember
    {
        std::optional<std::string> tempFileSuffix{std::nullopt};
        std::optional<bool> mayOverwrite{std::nullopt};
        std::optional<bool> tryContinue{std::nullopt};
        std::optional<bool> inheritPermissions{std::nullopt};
        std::optional<std::filesystem::perms> customFilePermissions{std::nullopt};
        std::optional<std::filesystem::perms> customDirectoryPermissions{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        CommonTransferOptions,
        (),
        (tempFileSuffix,
            mayOverwrite,
            tryContinue,
            inheritPermissions,
            customFilePermissions,
            customDirectoryPermissions)
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