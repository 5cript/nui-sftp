#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/settings/search/settings_search_index.hpp>
#include <persistence/state_holder.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief The search box in the settings header. It lists the best matching settings below itself; picking a
 * setting that exists in several places, like the inheritable defaults and every server, first asks where.
 */
class SettingsSearch
{
  public:
    /**
     * @brief Where to show a found setting.
     */
    struct Target
    {
        SettingScope scope{SettingScope::General};
        /**
         * @brief The server whose setting to show; only for the session scope.
         */
        std::optional<std::string> sessionId{};
    };

    /**
     * @brief Shows the setting at the target and highlights it.
     */
    using NavigateFunction = std::function<void(SettingsSearchIndex::Entry const& entry, Target const& target)>;

    /**
     * @param index The settings to search.
     * @param stateHolder The persisted state, for the list of servers and what they override.
     * @param events Tells whether settings are open and whether they finished rendering.
     * @param openSession Returns the server currently shown in the session editor, if any.
     * @param navigate Shows a picked setting.
     */
    SettingsSearch(
        SettingsSearchIndex& index,
        Persistence::StateHolder& stateHolder,
        FrontendEvents& events,
        std::function<std::optional<std::string>()> openSession,
        NavigateFunction navigate
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SettingsSearch);

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
