#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/number_setting.hpp>

#include <persistence/state/sftp_options.hpp>

#include <functional>

struct SftpOptions : public GroupKeys
{
    struct DownloadOptions
    {
        TextSetting<true> tempFileSuffix;
        BoolSetting<true> mayOverwrite;
        BoolSetting<true> tryContinue;
        BoolSetting<true> inheritPermissions;
        NumberSetting<unsigned short, true> customPermissions;
        BoolSetting<true> reserveSpace;
        BoolSetting<true> doCleanup;
    };
    struct UploadOptions
    {
        TextSetting<true> tempFileSuffix;
        BoolSetting<true> mayOverwrite;
        BoolSetting<true> tryContinue;
        BoolSetting<true> inheritPermissions;
        NumberSetting<unsigned short, true> customPermissions;
    };
    DownloadOptions downloadOptions;
    UploadOptions uploadOptions;
    TextSetting<true> defaultDirectory;

    Nui::Observed<bool> downloadOptionsEngaged{false};
    Nui::Observed<bool> uploadOptionsEngaged{false};

    NumberSetting<int, true> concurrency; // How many parallel transfers are allowed?
    NumberSetting<int> operationTimeoutSeconds;

    SftpOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::SftpOptions& state) const;
    void loadFromState(Persistence::SftpOptions const& state);
    void assumeDefaultsFrom(Persistence::SftpOptions const& state);
};