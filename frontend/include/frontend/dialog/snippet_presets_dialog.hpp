#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>
#include <string>

class CommandStoreClient;
class SnippetImportDialog;

/**
 * @brief Modal dialog with a card per shipped preset folder.
 *
 * A preset whose folder is missing is added right away. One whose folder exists opens the import
 * dialog on that folder, so the user reviews the differences before anything changes.
 */
class SnippetPresetsDialog
{
  public:
    SnippetPresetsDialog(std::string id, CommandStoreClient* client, SnippetImportDialog* importDialog);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SnippetPresetsDialog);

    /**
     * @brief Render hook; place this once near the other top-level dialogs.
     */
    Nui::ElementRenderer operator()();

    void open();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
