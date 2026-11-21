#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <frontend/settings/atomic_setting/list_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <utility/language.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief The settings of a remote server that do not depend on its protocol: where it is, its favorite directories
 * and how a lost connection is reconnected. Their texts lie below the language section of the protocol, so each
 * protocol keeps its own texts and html ids.
 */
struct RemoteServerSettings
{
    /**
     * @brief The values a reset restores, those of a new session.
     */
    struct Defaults
    {
        std::string host{};
        std::vector<std::string> remoteFavorites{};
        int maxReconnectAttempts{};
        int maxReconnectBackoffMs{};
    };

    TextSetting<> host;
    NumberSetting<int, true> port;
    ListSetting<> remoteFavorites;
    NumberSetting<int, false> maxReconnectAttempts;
    NumberSetting<int, false> maxReconnectBackoffMs;

    /**
     * @param connectionFactory Creates the identities of the host and the port.
     * @param miscellaneousFactory Creates the identities of the favorites and the reconnect settings.
     * @param languageSection The section below "settings" that holds the texts, like "sessionOptions".
     */
    RemoteServerSettings(
        SettingFactory const& connectionFactory,
        SettingFactory const& miscellaneousFactory,
        std::string const& languageSection,
        Defaults const& defaults,
        std::function<void()> const& onChange,
        InputDialog& inputDialog
    );

    /**
     * @brief The defaults of a new session whose options are @p options.
     */
    static Defaults defaultsOf(auto const& options)
    {
        return {
            .host = options.host,
            .remoteFavorites = options.remoteFavorites,
            .maxReconnectAttempts = options.maxReconnectAttempts,
            .maxReconnectBackoffMs = options.maxReconnectBackoffMs,
        };
    }

    void applyToState(auto& state) const
    {
        assignIfValid(state.host, host);
        assignIfValid(state.port, port);
        state.remoteFavorites = remoteFavorites.value();
        assignIfValid(state.maxReconnectAttempts, maxReconnectAttempts);
        assignIfValid(state.maxReconnectBackoffMs, maxReconnectBackoffMs);
    }

    void loadFromState(auto const& state)
    {
        host.value(state.host);
        port.value(state.port);
        remoteFavorites.value(state.remoteFavorites);
        maxReconnectAttempts.value(state.maxReconnectAttempts);
        maxReconnectBackoffMs.value(state.maxReconnectBackoffMs);
    }
};

/**
 * @brief Resets the users of a server only after a confirmation, because a reset removes all of them.
 *
 * @param languageSection The section below "settings" that holds the texts of the confirmation.
 */
auto confirmedUsersReset(
    auto& users,
    auto defaultUsers,
    std::string languageSection,
    std::function<void()> onChange,
    ConfirmDialog& confirmDialog
)
{
    return [&users,
            defaultUsers = std::move(defaultUsers),
            languageSection = std::move(languageSection),
            onChange = std::move(onChange),
            &confirmDialog]()
    {
        confirmDialog.open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("settings", languageSection.c_str(), "resetUsersConfirmHeader"),
            .text = language->get("settings", languageSection.c_str(), "resetUsersConfirmText"),
            .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
            .onClose = [&users, defaultUsers, onChange](std::optional<ConfirmDialog::Button> button)
            {
                if (button != ConfirmDialog::Button::Ok)
                    return;
                users.value(defaultUsers);
                onChange();
            },
        });
    };
}
