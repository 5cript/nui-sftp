#pragma once

#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/number_setting.hpp>

#include <functional>

struct SftpOptions
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

    Nui::Observed<bool> downloadOptionsEngaged;
    Nui::Observed<bool> uploadOptionsEngaged;

    NumberSetting<int, true> concurrency; // How many parallel transfers are allowed?
    NumberSetting<int> operationTimeoutSeconds;

    Nui::Observed<std::string> groupKey{"default"};
    Nui::Observed<std::vector<std::string>> groupKeys{{"default"}};

    SftpOptions(std::function<void()> const& onChange);
};