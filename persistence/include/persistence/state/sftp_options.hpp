#pragma once

#include <persistence/state_core.hpp>

#include <string>
#include <optional>
#include <filesystem>

namespace Persistence
{
    struct CommonTransferOptions
    {
        std::optional<std::string> tempFileSuffix{std::nullopt};
        std::optional<bool> mayOverwrite{std::nullopt};
        std::optional<bool> tryContinue{std::nullopt};
        std::optional<bool> inheritPermissions{std::nullopt};
        std::optional<std::filesystem::perms> customPermissions{std::nullopt};

        void useDefaultsFrom(CommonTransferOptions const& other);
    };
    void to_json(nlohmann::json& j, CommonTransferOptions const& options);
    void from_json(nlohmann::json const& j, CommonTransferOptions& options);

    struct DownloadOptions : public CommonTransferOptions
    {
        std::optional<bool> reserveSpace{std::nullopt};
        std::optional<bool> doCleanup{std::nullopt};

        void useDefaultsFrom(DownloadOptions const& other);
    };
    void to_json(nlohmann::json& j, DownloadOptions const& options);
    void from_json(nlohmann::json const& j, DownloadOptions& options);

    struct UploadOptions : public CommonTransferOptions
    {
        void useDefaultsFrom(UploadOptions const& other);
    };
    void to_json(nlohmann::json& j, UploadOptions const& options);
    void from_json(nlohmann::json const& j, UploadOptions& options);

    struct SftpOptions
    {
        std::optional<DownloadOptions> downloadOptions{};
        std::optional<UploadOptions> uploadOptions{};
        std::optional<int> concurrency{std::nullopt}; // How many parallel transfers are allowed?
        std::chrono::seconds operationTimeout{5};

        void useDefaultsFrom(SftpOptions const& other);
    };
    void to_json(nlohmann::json& j, SftpOptions const& options);
    void from_json(nlohmann::json const& j, SftpOptions& options);
}