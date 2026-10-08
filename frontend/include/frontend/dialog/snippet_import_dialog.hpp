#pragma once

#include <command-store/snippet_import_plan.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>
#include <string>

class CommandStoreClient;

/**
 * @brief Two step modal dialog that imports pasted snippet JSON.
 *
 * The first step takes the JSON and the target, the second shows what happens to every pasted
 * snippet and lets the user decide each name conflict before anything is stored.
 */
class SnippetImportDialog
{
  public:
    SnippetImportDialog(std::string id, CommandStoreClient* client);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SnippetImportDialog);

    /**
     * @brief Render hook; place this once near the other top-level dialogs.
     */
    Nui::ElementRenderer operator()();

    /** @brief Where the snippets come from; names the incoming side of a conflict. */
    enum class Source
    {
        Pasted,
        Preset,
    };

    struct OpenOptions
    {
        /** @brief The preselected target; the user can change it in the dialog. */
        CommandStore::SnippetImport::Target target{};

        /** @brief JSON the editor starts with instead of being empty. */
        std::string text{};

        /** @brief Starts at the review step when the text holds snippets. */
        bool startAtReview{false};

        Source source{Source::Pasted};
    };

    /**
     * @brief Opens the dialog, by default at its first step with an empty editor.
     */
    void open(OpenOptions options);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
