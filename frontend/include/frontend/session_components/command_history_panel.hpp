#pragma once

#include <frontend/command_store/command_store_client.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <string>

/**
 * @brief Dockable panel listing every command run through the command store.
 *
 * Shows a searchable, day-grouped list with a pinned strip, host filter pills, a favorites
 * filter and per-row actions (run, edit and run, copy, favorite, pin, delete). Binds to the
 * process global CommandStoreClient's observed history list.
 */
class CommandHistoryPanel
{
  public:
    /**
     * @param commandStoreClient The process global command store client; not owned.
     * @param events Frontend event hub; not owned.
     * @param confirmDialog Dialog for destructive confirmations; not owned.
     * @param runInTerminal Sends a command to the owning session's last interacted terminal.
     *                      The bool decides whether the command runs immediately (true) or is
     *                      only inserted for editing (false).
     */
    CommandHistoryPanel(
        CommandStoreClient* commandStoreClient,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        std::function<void(std::string const&, bool)> runInTerminal
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(CommandHistoryPanel);

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
