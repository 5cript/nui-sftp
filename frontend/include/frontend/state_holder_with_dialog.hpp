#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>

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
            std::optional<std::string> const& warning
        )
        {
            if (error)
            {
                const std::string extraMessage = extraErrorMessage.value_or("");

                confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "Error loading state",
                    .text = fmt::format(
                        "An error occurred while loading the application state: {}\n{}\nDefault state will be used.",
                        *error,
                        extraMessage
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return onLoad(false, holder.stateCache().fullyResolve());
            }

            if (warning)
            {
                confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                    .headerText = "Warning loading state",
                    .text = fmt::format(
                        "The application state was loaded with warnings:\n{}\nPlease check your configuration.",
                        *warning
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