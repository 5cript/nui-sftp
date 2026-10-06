#pragma once

#include <utility/language.hpp>

#include <script-nui-components/dialog.hpp>

/**
 * @brief The dialog button labels in the current language.
 */
inline ScriptNuiComponents::Dialog::ButtonLabels localizedButtonLabels()
{
    return {
        .ok = language->get("confirmDialog", "ok"),
        .cancel = language->get("confirmDialog", "cancel"),
        .yes = language->get("confirmDialog", "yes"),
        .no = language->get("confirmDialog", "no"),
        .all = language->get("confirmDialog", "all"),
        .none = language->get("confirmDialog", "none"),
    };
}
