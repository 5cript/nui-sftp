#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/persistence_load_warnings.hpp>
#include <utility/language.hpp>

inline void loadState(
    Persistence::StateHolder& stateHolder,
    ConfirmDialog* confirmDialog,
    std::function<void(bool success, Persistence::State const& stateHolder)> const& onLoad,
    std::optional<std::string> const& extraErrorMessage = std::nullopt
)
{
    stateHolder.load(
        [confirmDialog, onLoad, extraErrorMessage](
            std::optional<std::string> const& error,
            Persistence::StateHolder& holder,
            Persistence::LoadWarnings const& warnings
        )
        {
            if (error)
            {
                confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("persistence", "errorLoadingState"),
                    .text = fmt::format(
                        fmt::runtime(language->get("persistence", "loadFailed")), *error, extraErrorMessage.value_or("")
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return onLoad(false, holder.stateCache().fullyResolve());
            }

            if (!warnings.empty())
            {
                confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                    .headerText = language->get("persistence", "warningLoadingState"),
                    .text = fmt::format(
                        fmt::runtime(language->get("persistence", "loadedWithWarnings")), formatLoadWarnings(warnings)
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "persistenceLoadWarning",
                });
                holder.clearWarnings();
            }
            return onLoad(true, holder.stateCache().fullyResolve());
        }
    );
}