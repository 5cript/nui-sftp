#pragma once

#include <frontend/command_store/command_store_client.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/snippet_import_dialog.hpp>
#include <frontend/events/frontend_events.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <string>

/**
 * @brief Dockable panel managing saved command snippets.
 *
 * Shows a folder sidebar (all, favorites, user folders), a searchable card grid with tag filter
 * chips, a slide-over editor and an inline variable fill form for snippets with {{variable}}
 * placeholders. Binds to the process global CommandStoreClient's observed snippet and folder
 * lists.
 */
class CommandSnippetsPanel
{
  public:
    /**
     * @param commandStoreClient The process global command store client; not owned.
     * @param events Frontend event hub; not owned.
     * @param confirmDialog Dialog for destructive confirmations; not owned.
     * @param snippetImportDialog Dialog for importing pasted snippets; not owned.
     * @param runInTerminal Sends a command to the owning session's last interacted terminal.
     *                      The bool decides whether the command runs immediately (true) or is
     *                      only inserted for editing (false).
     * @param connectionLost The owning session's lost connection state; disables everything that
     *                       acts on the terminal while true. Not owned, outlives the panel.
     */
    CommandSnippetsPanel(
        CommandStoreClient* commandStoreClient,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        SnippetImportDialog* snippetImportDialog,
        std::function<void(std::string const&, bool)> runInTerminal,
        Nui::Observed<bool>* connectionLost
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(CommandSnippetsPanel);

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
