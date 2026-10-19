#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/direct_connect_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/theme_controller.hpp>
#include <persistence/state_holder.hpp>

#include <nlohmann/json.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <map>
#include <string>

class SessionArea;
class Settings;
class NotificationCenter;

class Toolbar
{
  public:
    Toolbar(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        DirectConnectDialog* directConnectDialog,
        InputDialog* inputDialog,
        ThemeController& themeController
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Toolbar);

    void sessionArea(SessionArea& sessionArea);
    void settings(Settings& settings);
    void notificationCenter(NotificationCenter& center);

    Nui::ElementRenderer operator()();
    std::string selectedLayout() const;

  private:
    void connectLayoutsChanged();
    void reloadLayouts();
    /**
     * @brief Asks for a name and saves the layout of the active session for all sessions of its type.
     */
    void saveLayout();
    /**
     * @brief Stores a layout under a name in the layouts of a session type and saves the settings.
     *
     * @param type The session type that gets the layout.
     * @param layoutName The name to store the layout under, replacing a layout of the same name.
     * @param layout The layout to store.
     */
    void storeLayout(Persistence::TerminalEngineType type, std::string const& layoutName, nlohmann::json const& layout);
    /**
     * @brief Asks for confirmation and removes the selected layout from the type of the selected server.
     */
    void deleteLayout();
    /**
     * @brief Changes the layouts of a session type in freshly loaded settings, saves them and reloads the dropdown.
     *
     * @param type The session type whose layouts change.
     * @param modifier Changes the layouts of the type.
     * @param selectAfterwards The layout to select once saved, if the selected server is still of that type.
     */
    void modifyLayouts(
        Persistence::TerminalEngineType type,
        std::function<void(std::map<std::string, nlohmann::json>&)> modifier,
        std::string const& selectAfterwards
    );

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};