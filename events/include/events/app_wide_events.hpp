#pragma once

#include <events/app_event_context.hpp>
#include <constants/persistence.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/tags.hpp>
#include <nui/event_system/tags_traits/sync_id.hpp>
#include <nui/synchronizer.hpp>
#include <nui/rpc.hpp>

#include <string>

struct AppWideEvents
{
    Nui::Observed<std::string, NUI_SYNCHRONIZE> onLanguageChanged{"en_US"};
    Nui::Observed<std::vector<std::filesystem::path>, NUI_SYNCHRONIZE> availableThemes{
        {std::string{Constants::defaultThemeName}}
    };
    Nui::Observed<bool, NUI_SYNCHRONIZE> onReloadThemes{false};

  private:
    Nui::Synchronizer<decltype(onLanguageChanged)> languageChangeSync;
    Nui::Synchronizer<decltype(availableThemes)> availableThemesSync;
    Nui::Synchronizer<decltype(onReloadThemes)> reloadThemesSync;

  public:
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
