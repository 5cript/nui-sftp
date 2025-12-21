#include <persistence/state/sftp_options.hpp>

namespace Persistence
{
    void CommonTransferOptions::useDefaultsFrom(CommonTransferOptions const& other)
    {
        if (!tempFileSuffix)
            tempFileSuffix = other.tempFileSuffix;
        if (!mayOverwrite)
            mayOverwrite = other.mayOverwrite;
        if (!tryContinue)
            tryContinue = other.tryContinue;
        if (!inheritPermissions)
            inheritPermissions = other.inheritPermissions;
        if (!customPermissions)
            customPermissions = other.customPermissions;
    }
    void to_json(nlohmann::json& j, CommonTransferOptions const& options)
    {
        if (options.tempFileSuffix)
            j["tempFileSuffix"] = *options.tempFileSuffix;
        if (options.mayOverwrite)
            j["mayOverwrite"] = *options.mayOverwrite;
        if (options.tryContinue)
            j["tryContinue"] = *options.tryContinue;
        if (options.inheritPermissions)
            j["inheritPermissions"] = *options.inheritPermissions;
        if (options.customPermissions)
            j["customPermissions"] = static_cast<unsigned int>(*options.customPermissions);
    }
    void from_json(nlohmann::json const& j, CommonTransferOptions& options)
    {
        if (j.contains("tempFileSuffix"))
            options.tempFileSuffix = j["tempFileSuffix"].template get<std::string>();
        if (j.contains("mayOverwrite"))
            options.mayOverwrite = j["mayOverwrite"].get<bool>();
        if (j.contains("tryContinue"))
            options.tryContinue = j["tryContinue"].get<bool>();
        if (j.contains("inheritPermissions"))
            options.inheritPermissions = j["inheritPermissions"].get<bool>();
        if (j.contains("customPermissions"))
            options.customPermissions = std::filesystem::perms{j["customPermissions"].template get<unsigned int>()};
    }

    void UploadOptions::useDefaultsFrom(UploadOptions const& other)
    {
        CommonTransferOptions::useDefaultsFrom(other);
    }
    void to_json(nlohmann::json& j, UploadOptions const& options)
    {
        to_json(j, static_cast<CommonTransferOptions const&>(options));
    }
    void from_json(nlohmann::json const& j, UploadOptions& options)
    {
        from_json(j, static_cast<CommonTransferOptions&>(options));
    }

    void DownloadOptions::useDefaultsFrom(DownloadOptions const& other)
    {
        CommonTransferOptions::useDefaultsFrom(other);
        if (!reserveSpace)
            reserveSpace = other.reserveSpace;
        if (!doCleanup)
            doCleanup = other.doCleanup;
    }
    void to_json(nlohmann::json& j, DownloadOptions const& options)
    {
        to_json(j, static_cast<CommonTransferOptions const&>(options));
        if (options.reserveSpace)
            j["reserveSpace"] = *options.reserveSpace;
        if (options.doCleanup)
            j["doCleanup"] = *options.doCleanup;
    }
    void from_json(nlohmann::json const& j, DownloadOptions& options)
    {
        from_json(j, static_cast<CommonTransferOptions&>(options));
        if (j.contains("reserveSpace"))
            options.reserveSpace = j["reserveSpace"].get<bool>();
        if (j.contains("doCleanup"))
            options.doCleanup = j["doCleanup"].get<bool>();
    }

    void to_json(nlohmann::json& j, SftpOptions const& options)
    {
        if (options.downloadOptions)
            j["downloadOptions"] = *options.downloadOptions;
        if (options.uploadOptions)
            j["uploadOptions"] = *options.uploadOptions;
        if (options.concurrency)
            j["concurrency"] = *options.concurrency;
        j["operationTimeout"] = options.operationTimeout.count();
    }
    void from_json(nlohmann::json const& j, SftpOptions& options)
    {
        if (j.contains("downloadOptions"))
            options.downloadOptions = j["downloadOptions"].get<DownloadOptions>();
        if (j.contains("uploadOptions"))
            options.uploadOptions = j["uploadOptions"].get<UploadOptions>();
        if (j.contains("concurrency"))
            options.concurrency = j["concurrency"].get<int>();

        if (j.contains("operationTimeout"))
            options.operationTimeout = std::chrono::seconds{j["operationTimeout"].get<int>()};
        else
            options.operationTimeout = SftpOptions{}.operationTimeout;
    }
    void SftpOptions::useDefaultsFrom(SftpOptions const& other)
    {
        if (!downloadOptions)
            downloadOptions = other.downloadOptions;
        if (!uploadOptions)
            uploadOptions = other.uploadOptions;
        if (!concurrency)
            concurrency = other.concurrency;
    }
}