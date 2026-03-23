#pragma once

#include <events/app_event_context.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/tags.hpp>
#include <nui/event_system/tags_traits/sync_id.hpp>
#include <nui/synchronizer.hpp>
#include <nui/rpc.hpp>

#include <string>

struct AppWideEvents
{
    Nui::Observed<std::string, SYNCHRONIZE> onLanguageChanged{"en_US"};
    Nui::Observed<std::vector<std::filesystem::path>, SYNCHRONIZE> availableThemes{{"default"}};
    Nui::Observed<bool, SYNCHRONIZE> onReloadThemes{false};

    Nui::Synchronizer<decltype(onLanguageChanged)> languageChangeSync;
    Nui::Synchronizer<decltype(availableThemes)> availableThemesSync;
    Nui::Synchronizer<decltype(onReloadThemes)> reloadThemesSync;

#ifdef NUI_FRONTEND
    AppWideEvents()
        : languageChangeSync{onLanguageChanged}
        , availableThemesSync{availableThemes}
        , reloadThemesSync{onReloadThemes}
    {}
#else
    AppWideEvents(Nui::RpcHub& rpcHub)
        : languageChangeSync{rpcHub, onLanguageChanged}
        , availableThemesSync{rpcHub, availableThemes}
        , reloadThemesSync{rpcHub, onReloadThemes}
    {}
#endif
};
