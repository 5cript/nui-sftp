#include <frontend/session_components/command_snippets_panel.hpp>
#include <frontend/session_components/command_panel_helpers.hpp>
#include <frontend/notifications.hpp>
#include <frontend/dialog/snippet_import_dialog.hpp>

#include <command-store/snippet_transfer.hpp>

#include <utility/command_template.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/pill_list.hpp>
#include <script-nui-components/tag_box.hpp>
#include <script-nui-components/checkbox.hpp>
#include <script-nui-components/select.hpp>
#include <ui5-sap-icons/icons/search.hpp>
#include <ui5-sap-icons/icons/favorite.hpp>
#include <ui5-sap-icons/icons/unfavorite.hpp>
#include <ui5-sap-icons/icons/media-play.hpp>
#include <ui5-sap-icons/icons/paste.hpp>
#include <ui5-sap-icons/icons/edit.hpp>
#include <ui5-sap-icons/icons/copy.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/decline.hpp>
#include <ui5-sap-icons/icons/add.hpp>
#include <ui5-sap-icons/icons/folder.hpp>
#include <ui5-sap-icons/icons/inbox.hpp>
#include <ui5-sap-icons/icons/list.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/event_system/observed_value_combinator.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace std::string_literals;

namespace
{
    using CommandPanels::lowercased;
    using CommandPanels::matchesSearch;
    using CommandPanels::relativeTime;

    /// Sidebar selection: one of the sentinels or a folder id prefixed with "folder:".
    constexpr char const* selectionAll = "all";
    constexpr char const* selectionFavorites = "favorites";
    constexpr char const* selectionUnfiled = "unfiled";

    /// A card with everything it displays, so an unchanged card compares equal.
    struct SnippetCard
    {
        CommandStore::Snippet snippet{};
        std::string timeLabel{};
        std::string loweredQuery{};

        bool operator==(SnippetCard const&) const = default;
    };

    std::string folderSelectionKey(std::string const& folderId)
    {
        return fmt::format("folder:{}", folderId);
    }

    /// Command preview with every {{variable}} token wrapped in a highlight span.
    Nui::ElementRenderer commandPreview(std::string const& command)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;

        std::vector<Nui::ElementRenderer> parts{};
        std::size_t position = 0;
        while (position < command.size())
        {
            const auto open = command.find("{{", position);
            if (open == std::string::npos)
                break;
            const auto close = command.find("}}", open + 2);
            if (close == std::string::npos)
                break;
            if (open > position)
                parts.push_back(text{command.substr(position, open - position)}());
            parts.push_back(span{class_ = "cmds-variable"}(command.substr(open, close + 2 - open)));
            position = close + 2;
        }
        if (position < command.size())
            parts.push_back(text{command.substr(position)}());

        return span{class_ = "cmds-preview"}(
            Nui::range(std::move(parts)),
            [](long long, auto const& part) -> Nui::ElementRenderer {
                return part;
            }
        );
    }

    /// The autofocus attribute is ignored for elements inserted after page load, so focus explicitly
    /// one frame after materialization, once the element is attached.
    Nui::Attribute focusWhenMounted(bool enabled = true)
    {
        return Nui::Attributes::reference.onMaterialize([enabled](Nui::val element) {
            if (!enabled)
                return;
            Nui::val::global("requestAnimationFrame")(Nui::bind(
                [element](Nui::val) {
                    element.call<void>("focus");
                },
                std::placeholders::_1
            ));
        });
    }

    /// Like focusWhenMounted, with the text selected so typing replaces it.
    Nui::Attribute selectWhenMounted()
    {
        return Nui::Attributes::reference.onMaterialize([](Nui::val element) {
            Nui::val::global("requestAnimationFrame")(Nui::bind(
                [element](Nui::val) {
                    element.call<void>("focus");
                    element.call<void>("select");
                },
                std::placeholders::_1
            ));
        });
    }

    std::string trimmed(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return {};
        const auto last = text.find_last_not_of(" \t\r\n");
        return std::string{text.substr(first, last - first + 1)};
    }

    void copyToClipboard(std::string const& textToCopy)
    {
        Nui::val::global("navigator")["clipboard"].call<Nui::val>("writeText", textToCopy);
    }
}

struct CommandSnippetsPanel::Implementation
{
    CommandStoreClient* client;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;
    SnippetImportDialog* snippetImportDialog;
    std::function<void(std::string const&, bool)> runInTerminal;
    Nui::Observed<bool>* connectionLost;

    Nui::Observed<std::string> searchQuery{};
    Nui::Observed<std::string> folderSelection{selectionAll};
    Nui::Observed<std::set<std::string>> selectedTags{};
    Nui::Observed<bool> newFolderInputVisible{false};
    /// The folder whose name is being edited in the sidebar; empty while none is.
    Nui::Observed<std::string> renamingFolderId{};

    /// Editor state; the plain members seed the inputs when the editor subtree is generated.
    Nui::Observed<bool> editorVisible{false};
    std::string editorId{};
    /// Observed so the validation messages can follow the inputs live.
    Nui::Observed<std::string> editorName{};
    /// Validation stays quiet until the first save attempt, then follows every edit.
    Nui::Observed<bool> editorValidationShown{false};
    std::string editorFolderId{};
    bool editorFavorite{false};
    /// Folder choices of the select, (label, id), rebuilt when the editor opens.
    std::vector<std::pair<std::string, std::string>> editorFolderChoices{};
    /// The chosen (label, id); the id tells folders of the same name apart.
    Nui::Observed<std::pair<std::string, std::string>> editorFolderChoice{};
    /// Observed so the detected-variables badges can follow the textarea live.
    Nui::Observed<std::string> editorCommand{};
    ScriptNuiComponents::TagBox editorTags{{
        .placeholder = std::string{language->get("commandSnippetsPanel", "tagsPlaceholder")},
    }};

    /// Variable fill form state; values are plain, only the preview is live.
    Nui::Observed<bool> variableFormVisible{false};
    std::string variableFormSnippetId{};
    std::string variableFormCommand{};
    std::vector<std::string> variableFormVariables{};
    std::map<std::string, std::string> variableFormValues{};
    Nui::Observed<std::string> variableFormPreview{};
    /// Which card button opened the form; decides the primary button and what Enter does.
    bool variableFormExecute{true};

    std::shared_ptr<Nui::Observed<std::vector<ScriptNuiComponents::PillOptions>>> tagPills{
        std::make_shared<Nui::Observed<std::vector<ScriptNuiComponents::PillOptions>>>()
    };
    /// What the grid shows, derived from the snippets and the filters by refreshCards.
    Nui::Observed<std::vector<SnippetCard>> cards{};
    Nui::ListenRemover<Nui::Observed<std::vector<CommandStore::Snippet>>> snippetsListener{};
    Nui::ListenRemover<Nui::Observed<std::vector<CommandStore::SnippetFolder>>> foldersListener{};
    Nui::ListenRemover<Nui::Observed<std::string>> searchListener{};
    Nui::ListenRemover<Nui::Observed<std::string>> folderSelectionListener{};
    Nui::ListenRemover<Nui::Observed<std::set<std::string>>> tagsListener{};

    Implementation(
        CommandStoreClient* client,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        SnippetImportDialog* snippetImportDialog,
        std::function<void(std::string const&, bool)> runInTerminal,
        Nui::Observed<bool>* connectionLost
    )
        : client{client}
        , events{events}
        , confirmDialog{confirmDialog}
        , snippetImportDialog{snippetImportDialog}
        , runInTerminal{std::move(runInTerminal)}
        , connectionLost{connectionLost}
    {}

    void rebuildTagPills()
    {
        std::set<std::string> allTags{};
        for (auto const& snippet : client->snippets().value())
            allTags.insert(snippet.tags.begin(), snippet.tags.end());

        // Drop filter tags that no longer exist on any snippet.
        std::erase_if(selectedTags.value(), [&allTags](std::string const& tag) {
            return !allTags.contains(tag);
        });

        std::vector<ScriptNuiComponents::PillOptions> pills{};
        pills.reserve(allTags.size());
        for (auto const& tag : allTags)
        {
            pills.push_back(ScriptNuiComponents::PillOptions{
                .text = tag,
                .selected = selectedTags.value().contains(tag),
                .onClick =
                    [this, tag]() {
                        auto& tags = selectedTags.value();
                        if (!tags.erase(tag))
                            tags.insert(tag);
                        selectedTags.modify();
                        rebuildTagPills();
                    },
            });
        }
        *tagPills = std::move(pills);
    }

    /// Snippets without a folder, or whose folder no longer exists, count as unfiled.
    bool isUnfiled(CommandStore::Snippet const& snippet) const
    {
        return snippet.folder.empty() ||
            std::ranges::none_of(client->folders().value(), [&snippet](CommandStore::SnippetFolder const& folder) {
                   return folder.id == snippet.folder;
               });
    }

    /// The snippets passing folder, tag and search filters, ordered by name (client order).
    std::vector<CommandStore::Snippet const*> visibleSnippets() const
    {
        const auto loweredQuery = lowercased(searchQuery.value());
        const auto& selection = folderSelection.value();

        std::vector<CommandStore::Snippet const*> visible{};
        for (auto const& snippet : client->snippets().value())
        {
            if (selection == selectionFavorites)
            {
                if (!snippet.favorite)
                    continue;
            }
            else if (selection == selectionUnfiled)
            {
                if (!isUnfiled(snippet))
                    continue;
            }
            else if (selection != selectionAll && folderSelectionKey(snippet.folder) != selection)
                continue;

            const bool hasAllTags = std::ranges::all_of(selectedTags.value(), [&snippet](std::string const& tag) {
                return std::ranges::find(snippet.tags, tag) != snippet.tags.end();
            });
            if (!hasAllTags)
                continue;

            if (!loweredQuery.empty())
            {
                const bool tagMatch = std::ranges::any_of(snippet.tags, [&loweredQuery](std::string const& tag) {
                    return matchesSearch(loweredQuery, tag);
                });
                if (!matchesSearch(loweredQuery, snippet.name) && !matchesSearch(loweredQuery, snippet.command) &&
                    !tagMatch)
                    continue;
            }

            visible.push_back(&snippet);
        }
        return visible;
    }

    void refreshCards()
    {
        const auto loweredQuery = lowercased(searchQuery.value());
        const auto nowEpoch = static_cast<std::int64_t>(std::time(nullptr));
        std::vector<SnippetCard> target{};
        for (auto const* snippet : visibleSnippets())
        {
            target.push_back(SnippetCard{
                .snippet = *snippet,
                .timeLabel = snippet->lastUsed > 0 ? relativeTime(snippet->lastUsed, nowEpoch) : std::string{},
                .loweredQuery = loweredQuery,
            });
        }
        CommandPanels::updateKeyed(cards, std::move(target), [](SnippetCard const& card) -> std::string const& {
            return card.snippet.id;
        });
    }

    void openEditor(CommandStore::Snippet const& snippet)
    {
        editorId = snippet.id;
        editorName = snippet.name;
        editorFolderId = snippet.folder;
        editorFavorite = snippet.favorite;
        editorCommand = snippet.command;
        editorTags.tags(snippet.tags);

        editorFolderChoices.clear();
        editorFolderChoices.emplace_back(std::string{language->get("commandSnippetsPanel", "unfiled")}, "");
        for (auto const& folder : client->folders().value())
            editorFolderChoices.emplace_back(folder.name, folder.id);
        const auto active = std::ranges::find_if(editorFolderChoices, [this](auto const& choice) {
            return choice.second == editorFolderId;
        });
        editorFolderChoice = active != editorFolderChoices.end() ? *active : editorFolderChoices.front();

        editorValidationShown = false;
        editorVisible = true;
    }

    /// The reason the name blocks saving, or an empty string.
    std::string editorNameError() const
    {
        if (trimmed(editorName.value()).empty())
            return std::string{language->get("commandSnippetsPanel", "nameRequired")};
        return {};
    }

    /// The reason the command blocks saving, or an empty string.
    std::string editorCommandError() const
    {
        const auto& command = editorCommand.value();
        if (trimmed(command).empty())
            return std::string{language->get("commandSnippetsPanel", "commandRequired")};
        const auto unmatched = Utility::CommandTemplate::findUnmatchedBrace(command);
        if (!unmatched)
            return {};
        constexpr std::size_t excerptLength = 24u;
        auto excerptEnd = std::min(command.size(), *unmatched + excerptLength);
        // Never cut a UTF-8 sequence in half.
        while (excerptEnd < command.size() && (static_cast<unsigned char>(command[excerptEnd]) & 0xC0u) == 0x80u)
            --excerptEnd;
        const auto excerpt = command.substr(*unmatched, excerptEnd - *unmatched);
        return fmt::format(
            fmt::runtime(language->get("commandSnippetsPanel", "unmatchedBraces")),
            excerptEnd < command.size() ? fmt::format("{}...", excerpt) : excerpt
        );
    }

    std::string fieldClass(std::string_view baseClass, std::string const& error) const
    {
        if (editorValidationShown.value() && !error.empty())
            return fmt::format("{} cmds-field-invalid", baseClass);
        return std::string{baseClass};
    }

    Nui::ElementRenderer fieldError(std::string const& error) const
    {
        if (!editorValidationShown.value() || error.empty())
            return Nui::nil();
        return Nui::Elements::div{Nui::Attributes::class_ = "cmds-field-error"}(error);
    }

    void saveEditor()
    {
        editorValidationShown = true;
        if (!editorNameError().empty() || !editorCommandError().empty())
            return;

        client->upsertSnippet(CommandStore::Snippet{
            .id = editorId,
            .name = trimmed(editorName.value()),
            .command = editorCommand.value(),
            .folder = editorFolderId,
            .tags = editorTags.tags(),
            .favorite = editorFavorite,
        });
        editorVisible = false;
    }

    /** @brief Sends the snippet to the terminal, through the variable form if it has variables.
     *  @param execute Runs the command when true, otherwise only types it for editing.
     */
    void sendSnippet(CommandStore::Snippet const& snippet, bool execute)
    {
        const auto variables = Utility::CommandTemplate::parseVariables(snippet.command);
        if (variables.empty())
        {
            runInTerminal(snippet.command, execute);
            client->bumpSnippetUse(snippet.id);
            return;
        }

        variableFormSnippetId = snippet.id;
        variableFormCommand = snippet.command;
        variableFormVariables = variables;
        variableFormValues.clear();
        variableFormPreview = snippet.command;
        variableFormExecute = execute;
        variableFormVisible = true;
    }

    bool variableFormComplete() const
    {
        return std::ranges::all_of(variableFormVariables, [this](std::string const& variable) {
            const auto value = variableFormValues.find(variable);
            return value != variableFormValues.end() && !value->second.empty();
        });
    }

    void submitVariableForm(bool execute)
    {
        // Enter in a field submits too, so the disabled buttons alone do not cover it.
        if (connectionLost->value() || (execute && !variableFormComplete()))
            return;
        runInTerminal(Utility::CommandTemplate::substitute(variableFormCommand, variableFormValues), execute);
        client->bumpSnippetUse(variableFormSnippetId);
        variableFormVisible = false;
    }

    void deleteSnippet(CommandStore::Snippet const& snippet)
    {
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = std::string{language->get("commandSnippetsPanel", "confirmDeleteHeader")},
            .text = fmt::format(
                fmt::runtime(std::string{language->get("commandSnippetsPanel", "confirmDeleteText")}), snippet.name
            ),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .onClose =
                [this, id = snippet.id](std::optional<ConfirmDialog::Button> button) {
                    if (!button || *button != ConfirmDialog::Button::Yes)
                        return;
                    client->deleteSnippet(id);
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
        });
    }

    void deleteFolder(CommandStore::SnippetFolder const& folder)
    {
        const auto containedCount = std::ranges::count_if(
            client->snippets().value(),
            [&folder](CommandStore::Snippet const& snippet) {
                return snippet.folder == folder.id;
            }
        );
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = std::string{language->get("commandSnippetsPanel", "confirmDeleteFolderHeader")},
            .text = fmt::format(
                fmt::runtime(std::string{language->get("commandSnippetsPanel", "confirmDeleteFolderText")}),
                folder.name,
                containedCount
            ),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .onClose =
                [this, id = folder.id](std::optional<ConfirmDialog::Button> button) {
                    if (!button || *button != ConfirmDialog::Button::Yes)
                        return;
                    if (folderSelection.value() == folderSelectionKey(id))
                        folderSelection = selectionAll;
                    // The folder's snippets go with it; the store alone would only orphan them
                    // into the root.
                    std::vector<std::string> containedIds{};
                    for (auto const& snippet : client->snippets().value())
                    {
                        if (snippet.folder == id)
                            containedIds.push_back(snippet.id);
                    }
                    for (auto const& snippetId : containedIds)
                        client->deleteSnippet(snippetId);
                    client->deleteFolder(id);
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
        });
    }

    void createFolder(std::string const& name)
    {
        if (name.empty())
            return;
        auto position = std::int64_t{0};
        for (auto const& folder : client->folders().value())
            position = std::max(position, folder.position + 1);
        client->upsertFolder(CommandStore::SnippetFolder{.name = name, .position = position});
        newFolderInputVisible = false;
    }

    void startRename(std::string const& folderId)
    {
        const auto& folders = client->folders().value();
        if (std::ranges::none_of(folders, [&folderId](CommandStore::SnippetFolder const& folder) {
                return folder.id == folderId;
            }))
            return;
        newFolderInputVisible = false;
        renamingFolderId = folderId;
    }

    /// Enter and losing focus both commit; whichever comes second finds nothing left to do.
    void commitRename(std::string const& folderId, std::string const& newName)
    {
        if (renamingFolderId.value() != folderId)
            return;
        renamingFolderId = std::string{};

        const auto name = trimmed(newName);
        const auto& folders = client->folders().value();
        const auto folder = std::ranges::find_if(folders, [&folderId](CommandStore::SnippetFolder const& candidate) {
            return candidate.id == folderId;
        });
        if (name.empty() || folder == folders.end() || folder->name == name)
            return;
        auto renamed = *folder;
        renamed.name = name;
        client->upsertFolder(std::move(renamed));
    }

    /// Copies the shown snippets as JSON, with folders by name so an import can recreate them.
    void copyVisibleSnippets() const
    {
        const auto& folders = client->folders().value();
        const auto visible = visibleSnippets();
        std::vector<CommandStore::TransferSnippet> exported{};
        exported.reserve(visible.size());
        for (auto const* snippet : visible)
        {
            const auto folder = std::ranges::find_if(folders, [snippet](CommandStore::SnippetFolder const& candidate) {
                return candidate.id == snippet->folder;
            });
            exported.push_back(CommandStore::TransferSnippet{
                .name = snippet->name,
                .command = snippet->command,
                .folder = folder != folders.end() ? folder->name : std::string{},
                .tags = snippet->tags,
                .favorite = snippet->favorite,
            });
        }
        const auto document = CommandStore::SnippetTransfer::toJson(exported);

        Nui::val::global("navigator")["clipboard"]
            .call<Nui::val>("writeText", document.dump(2))
            .call<void>(
                "then",
                Nui::bind(
                    [count = visible.size()](Nui::val) {
                        Notifications::success(fmt::format(
                            fmt::runtime(language->get(
                                "commandSnippetsPanel", count == 1 ? "snippetsCopiedOne" : "snippetsCopiedMany"
                            )),
                            count
                        ));
                    },
                    std::placeholders::_1
                ),
                Nui::bind(
                    [](Nui::val error) {
                        Notifications::error(fmt::format(
                            fmt::runtime(language->get("commandSnippetsPanel", "copySnippetsFailed")),
                            error.call<std::string>("toString")
                        ));
                    },
                    std::placeholders::_1
                )
            );
    }

    /**
     * @brief Imports into the selected folder or Unfiled; All and Favorites take the folders the
     *        pasted JSON names.
     */
    void openImport()
    {
        auto const& selection = folderSelection.value();
        auto target = CommandStore::SnippetImport::Target{};
        if (selection == selectionUnfiled)
            target.folderId = std::string{};
        else if (selection.starts_with("folder:"))
            target.folderId = selection.substr(7);
        snippetImportDialog->open({.target = std::move(target)});
    }

    Nui::ElementRenderer renderSidebar();
    Nui::ElementRenderer renderCard(SnippetCard const& card);
    Nui::ElementRenderer renderEditor();
    Nui::ElementRenderer renderVariableForm();
};

Nui::ElementRenderer CommandSnippetsPanel::Implementation::renderSidebar()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    const auto& selection = folderSelection.value();
    const auto& snippets = client->snippets().value();

    auto favoriteCount = std::size_t{0};
    auto unfiledCount = std::size_t{0};
    std::map<std::string, std::size_t> folderCounts{};
    for (auto const& snippet : snippets)
    {
        if (snippet.favorite)
            ++favoriteCount;
        if (isUnfiled(snippet))
            ++unfiledCount;
        else
            ++folderCounts[snippet.folder];
    }

    // The count and the optional hover action share one fixed slot so counts line up across rows.
    const auto sidebarEntry = [this, &selection](
                                  std::string const& key,
                                  Nui::ElementRenderer icon,
                                  std::string const& label,
                                  std::size_t count,
                                  Nui::ElementRenderer hoverAction
                              ) -> Nui::ElementRenderer {
        return div{
            class_ = selection == key ? "cmds-folder cmds-folder-selected" : "cmds-folder",
            onClick =
                [this, key](Nui::val) {
                    folderSelection = key;
                },
        }(
            std::move(icon),
            span{class_ = "cmds-folder-name"}(label),
            span{class_ = "cmds-folder-trailing"}(
                span{class_ = "cmds-folder-count"}(fmt::format("{}", count)), std::move(hoverAction)
            )
        );
    };

    std::vector<Nui::ElementRenderer> entries{};
    entries.reserve(client->folders().value().size() + 4);
    entries.push_back(sidebarEntry(
        selectionAll,
        Ui5Icons::list(),
        std::string{language->get("commandSnippetsPanel", "allSnippets")},
        snippets.size(),
        Nui::nil()
    ));
    entries.push_back(sidebarEntry(
        selectionFavorites,
        Ui5Icons::favorite(),
        std::string{language->get("commandSnippetsPanel", "favorites")},
        favoriteCount,
        Nui::nil()
    ));
    entries.push_back(div{class_ = "cmds-sidebar-separator"}());

    for (auto const& folder : client->folders().value())
    {
        if (renamingFolderId.value() == folder.id)
        {
            entries.push_back(div{class_ = "cmds-folder cmds-folder-renaming"}(
                Ui5Icons::folder(),
                input{
                    type = "text",
                    class_ = "cmds-folder-rename-input",
                    value = folder.name,
                    selectWhenMounted(),
                    onKeyDown =
                        [this, id = folder.id](Nui::val event) {
                            // Keeps the sidebar's F2 handler and anything further up out of typing.
                            event.call<void>("stopPropagation");
                            const auto key = event["key"].as<std::string>();
                            if (key == "Enter")
                                commitRename(id, event["target"]["value"].as<std::string>());
                            else if (key == "Escape")
                                renamingFolderId = std::string{};
                        },
                    onBlur =
                        [this, id = folder.id](Nui::val event) {
                            commitRename(id, event["target"]["value"].as<std::string>());
                        },
                }()
            ));
            continue;
        }

        const auto count = folderCounts.contains(folder.id) ? folderCounts.at(folder.id) : 0;
        entries.push_back(sidebarEntry(
            folderSelectionKey(folder.id),
            Ui5Icons::folder(),
            folder.name,
            count,
            span{class_ = "cmds-folder-actions"}(
                button{
                    class_ = "cmds-action",
                    title = language->get("commandSnippetsPanel", "renameFolderTooltip"),
                    onClick =
                        [this, id = folder.id](Nui::val event) {
                            event.call<void>("stopPropagation");
                            startRename(id);
                        },
                }(Ui5Icons::edit()),
                button{
                    class_ = "cmds-action cmds-action-danger",
                    title = language->get("commandSnippetsPanel", "deleteFolderTooltip"),
                    onClick =
                        [this, folder](Nui::val event) {
                            event.call<void>("stopPropagation");
                            deleteFolder(folder);
                        },
                }(Ui5Icons::delete_())
            )
        ));
    }
    entries.push_back(sidebarEntry(
        selectionUnfiled,
        Ui5Icons::inbox(),
        std::string{language->get("commandSnippetsPanel", "unfiled")},
        unfiledCount,
        Nui::nil()
    ));

    return div{class_ = "cmds-sidebar"}(
        div{class_ = "cmds-folder-list"}(
            Nui::range(std::move(entries)),
            [](long long, auto const& entry) -> Nui::ElementRenderer {
                return entry;
            }
        ),
        newFolderInputVisible.value()
            ? Nui::ElementRenderer{input{
                  type = "text",
                  class_ = "cmds-new-folder-input",
                  placeHolder = language->get("commandSnippetsPanel", "newFolderPlaceholder"),
                  focusWhenMounted(),
                  onKeyUp =
                      [this](Nui::val event) {
                          const auto key = event["key"].as<std::string>();
                          if (key == "Enter")
                              createFolder(event["target"]["value"].as<std::string>());
                          else if (key == "Escape")
                              newFolderInputVisible = false;
                      },
              }()}
            : Nui::ElementRenderer{button{
                  class_ = "cmds-new-folder",
                  onClick =
                      [this](Nui::val) {
                          newFolderInputVisible = true;
                      },
              }(Ui5Icons::add(), span{}(language->get("commandSnippetsPanel", "newFolder")))}
    );
}

Nui::ElementRenderer CommandSnippetsPanel::Implementation::renderCard(SnippetCard const& card)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    auto const& snippet = card.snippet;
    const auto variables = Utility::CommandTemplate::parseVariables(snippet.command);

    std::vector<Nui::ElementRenderer> tagPillElements{};
    tagPillElements.reserve(snippet.tags.size());
    for (auto const& tag : snippet.tags)
        tagPillElements.push_back(span{class_ = "cmds-tag"}(tag));

    // clang-format off
    return div{class_ = "cmds-card"}(
        div{class_ = "cmds-card-header"}(
            CommandPanels::highlightedText(snippet.name, card.loweredQuery, "cmds-card-name"),
            div{class_ = "cmds-card-actions"}(
                button{
                    class_ = "cmds-action",
                    title = language->get("commandSnippetsPanel", "editTooltip"),
                    onClick = [this, snippet](Nui::val) {
                        openEditor(snippet);
                    },
                }(Ui5Icons::edit()),
                button{
                    class_ = "cmds-action",
                    title = language->get("commandSnippetsPanel", "copyTooltip"),
                    onClick = [command = snippet.command](Nui::val) {
                        copyToClipboard(command);
                    },
                }(Ui5Icons::copy()),
                button{
                    class_ = "cmds-action cmds-action-danger",
                    title = language->get("commandSnippetsPanel", "deleteTooltip"),
                    onClick = [this, snippet](Nui::val) {
                        deleteSnippet(snippet);
                    },
                }(Ui5Icons::delete_()),
                // Last, so the always visible star of favorites sits in the card's corner.
                button{
                    class_ = snippet.favorite ? "cmds-action cmds-action-active cmds-action-favorited" : "cmds-action",
                    title = language->get("commandSnippetsPanel", "favoriteTooltip"),
                    onClick = [this, snippet](Nui::val) {
                        auto changed = snippet;
                        changed.favorite = !changed.favorite;
                        client->upsertSnippet(std::move(changed));
                    },
                }(snippet.favorite ? Ui5Icons::favorite() : Ui5Icons::unfavorite())
            )
        ),
        commandPreview(snippet.command),
        div{class_ = "cmds-card-footer"}(
            div{class_ = "cmds-card-tags"}(
                Nui::range(std::move(tagPillElements)),
                [](long long, auto const& tagPill) -> Nui::ElementRenderer {
                    return tagPill;
                }
            ),
            div{class_ = "cmds-card-meta"}(
                !variables.empty()
                    ? Nui::ElementRenderer{span{
                          class_ = "cmds-parameter-count",
                          title = fmt::format(
                              fmt::runtime(language->get("commandSnippetsPanel", "parameterListTooltip")),
                              fmt::join(variables, ", ")
                          ),
                      }(fmt::format(
                          fmt::runtime(language->get(
                              "commandSnippetsPanel", variables.size() == 1 ? "parameterCountOne" : "parameterCountMany"
                          )),
                          variables.size()
                      ))}
                    : Nui::nil(),
                !card.timeLabel.empty()
                    ? Nui::ElementRenderer{span{class_ = "cmds-last-used"}(card.timeLabel)}
                    : Nui::nil(),
                snippet.uses > 0
                    ? Nui::ElementRenderer{span{class_ = "cmds-uses"}(fmt::format("×{}", snippet.uses))}
                    : Nui::nil(),
                // Grouped, so on a narrow card they wrap onto their own line together.
                div{class_ = "cmds-card-buttons"}(
                    button{
                        class_ = "cmds-insert",
                        CommandPanels::connectionTooltip(
                            *connectionLost, std::string{language->get("commandSnippetsPanel", "insertTooltip")}),
                        CommandPanels::disabledWhileDisconnected(*connectionLost),
                        onClick = [this, snippet](Nui::val) {
                            sendSnippet(snippet, false);
                        },
                    }(Ui5Icons::paste(), span{}(language->get("commandSnippetsPanel", "insert"))),
                    button{
                        class_ = "cmds-run",
                        CommandPanels::connectionTooltip(
                            *connectionLost, std::string{language->get("commandSnippetsPanel", "runTooltip")}),
                        CommandPanels::disabledWhileDisconnected(*connectionLost),
                        onClick = [this, snippet](Nui::val) {
                            sendSnippet(snippet, true);
                        },
                    }(Ui5Icons::media_play(), span{}(language->get("commandSnippetsPanel", "run")))
                )
            )
        )
    );
    // clang-format on
}

Nui::ElementRenderer CommandSnippetsPanel::Implementation::renderEditor()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Elements::label;

    if (!editorVisible.value())
        return Nui::nil();

    // clang-format off
    return div{class_ = "cmds-editor-backdrop"}(
        div{class_ = "cmds-editor"}(
            div{class_ = "cmds-editor-title"}(
                language->get(
                    "commandSnippetsPanel", editorId.empty() ? "editorTitleNew" : "editorTitleEdit"
                )
            ),
            label{}(language->get("commandSnippetsPanel", "nameLabel")),
            input{
                type = "text",
                class_ = Nui::observe(editorName, editorValidationShown).generate([this]() {
                    return fieldClass("cmds-editor-name", editorNameError());
                }),
                value = editorName.value(),
                focusWhenMounted(),
                onInput = [this](Nui::val event) {
                    editorName = event["target"]["value"].as<std::string>();
                },
                // Keydown: the keyup of the Enter that opened the editor lands here as well.
                onKeyDown = [this](Nui::val event) {
                    if (event["key"].as<std::string>() == "Enter")
                        saveEditor();
                },
            }(),
            div{class_ = "cmds-field-error-host"}(
                Nui::observe(editorName, editorValidationShown).generate([this]() -> Nui::ElementRenderer {
                    return fieldError(editorNameError());
                })
            ),
            label{}(language->get("commandSnippetsPanel", "commandLabel")),
            textarea{
                class_ = Nui::observe(editorCommand, editorValidationShown).generate([this]() {
                    return fieldClass("cmds-editor-command", editorCommandError());
                }),
                onInput = [this](Nui::val event) {
                    editorCommand = event["target"]["value"].as<std::string>();
                },
            }(editorCommand.value()),
            div{class_ = "cmds-field-error-host"}(
                Nui::observe(editorCommand, editorValidationShown).generate([this]() -> Nui::ElementRenderer {
                    return fieldError(editorCommandError());
                })
            ),
            div{class_ = "cmds-editor-variables"}(
                Nui::observe(editorCommand).generate([this]() -> Nui::ElementRenderer {
                    const auto variables = Utility::CommandTemplate::parseVariables(editorCommand.value());
                    if (variables.empty())
                        return Nui::nil();
                    std::vector<Nui::ElementRenderer> badges{};
                    badges.reserve(variables.size());
                    for (auto const& variable : variables)
                        badges.push_back(span{class_ = "cmds-variable-badge"}(variable));
                    return div{class_ = "cmds-variable-badges"}(
                        Nui::range(std::move(badges)),
                        [](long long, auto const& badge) -> Nui::ElementRenderer {
                            return badge;
                        }
                    );
                })
            ),
            label{}(language->get("commandSnippetsPanel", "folderLabel")),
            ScriptNuiComponents::select(
                ScriptNuiComponents::SelectOptions<decltype(editorFolderChoice), decltype(editorFolderChoices)>{
                    .activeOption = editorFolderChoice,
                    .options = editorFolderChoices,
                    .onChange =
                        [this](std::pair<std::string, std::string> const& choice, Nui::WebApi::MouseEvent const&) {
                            editorFolderId = choice.second;
                        },
                    .activeRenderer =
                        [](std::reference_wrapper<Nui::Observed<std::pair<std::string, std::string>>>& active) {
                            return span{}(
                                Nui::observe(active.get()).generate([&observed = active.get()]() -> Nui::ElementRenderer {
                                    return Nui::Elements::text{observed.value().first}();
                                })
                            );
                        },
                    .elementRenderer =
                        [](std::pair<std::string, std::string> const& choice) {
                            return span{}(choice.first);
                        },
                }
            ),
            label{}(language->get("commandSnippetsPanel", "tagsLabel")),
            editorTags(),
            // No class_ in component attributes, it would override the component's own class.
            ScriptNuiComponents::checkbox({
                .isChecked = editorFavorite,
                .label = std::string{language->get("commandSnippetsPanel", "favoriteLabel")},
                .onChange =
                    [this](bool isChecked, Nui::WebApi::MouseEvent const&) {
                        editorFavorite = isChecked;
                    },
            }),
            div{class_ = "cmds-editor-buttons"}(
                button{
                    class_ = "cmds-button cmds-button-primary",
                    onClick = [this](Nui::val) {
                        saveEditor();
                    },
                }(language->get("commandSnippetsPanel", "save")),
                button{
                    class_ = "cmds-button",
                    onClick = [this](Nui::val) {
                        editorVisible = false;
                    },
                }(language->get("commandSnippetsPanel", "cancel"))
            )
        )
    );
    // clang-format on
}

Nui::ElementRenderer CommandSnippetsPanel::Implementation::renderVariableForm()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::label;

    if (!variableFormVisible.value())
        return Nui::nil();

    std::vector<Nui::ElementRenderer> fields{};
    fields.reserve(variableFormVariables.size());
    for (auto const& variable : variableFormVariables)
    {
        fields.push_back(div{class_ = "cmds-variable-field"}(
            label{}(variable),
            input{
                type = "text",
                focusWhenMounted(fields.empty()),
                onInput =
                    [this, variable](Nui::val event) {
                        variableFormValues[variable] = event["target"]["value"].as<std::string>();
                        variableFormPreview =
                            Utility::CommandTemplate::substitute(variableFormCommand, variableFormValues);
                    },
                // Keydown: the keyup of the Enter that opened the form lands here as well.
                onKeyDown =
                    [this](Nui::val event) {
                        const auto key = event["key"].as<std::string>();
                        if (key == "Enter")
                            submitVariableForm(variableFormExecute);
                        else if (key == "Escape")
                            variableFormVisible = false;
                    },
            }()
        ));
    }

    // Running needs every parameter; inserting may leave some for editing in the terminal. Neither
    // works without the connection. The preview changes on every keystroke, so it doubles as the
    // trigger for re-evaluation.
    const auto submitButton = [this](bool execute, std::string const& text) -> Nui::ElementRenderer {
        using Nui::Attributes::title;
        return button{
            class_ = execute == variableFormExecute ? "cmds-button cmds-button-primary" : "cmds-button",
            disabled = Nui::observe(variableFormPreview, *connectionLost).generate([this, execute]() {
                return connectionLost->value() || (execute && !variableFormComplete());
            }),
            title = Nui::observe(variableFormPreview, *connectionLost).generate([this, execute]() {
                if (connectionLost->value())
                    return std::string{language->get("commandPanels", "disconnectedTooltip")};
                if (execute && !variableFormComplete())
                    return std::string{language->get("commandSnippetsPanel", "runNeedsAllParameters")};
                return std::string{};
            }),
            onClick =
                [this, execute](Nui::val) {
                    submitVariableForm(execute);
                },
        }(text);
    };

    // clang-format off
    return div{class_ = "cmds-editor-backdrop"}(
        div{class_ = "cmds-editor cmds-variable-form"}(
            div{class_ = "cmds-editor-title"}(language->get("commandSnippetsPanel", "variableFormTitle")),
            div{class_ = "cmds-variable-fields"}(
                Nui::range(std::move(fields)),
                [](long long, auto const& field) -> Nui::ElementRenderer {
                    return field;
                }
            ),
            div{class_ = "cmds-variable-preview"}(variableFormPreview),
            div{class_ = "cmds-editor-buttons"}(
                submitButton(false, std::string{language->get("commandSnippetsPanel", "insert")}),
                submitButton(true, std::string{language->get("commandSnippetsPanel", "run")}),
                button{
                    class_ = "cmds-button",
                    onClick = [this](Nui::val) {
                        variableFormVisible = false;
                    },
                }(language->get("commandSnippetsPanel", "cancel"))
            )
        )
    );
    // clang-format on
}

CommandSnippetsPanel::CommandSnippetsPanel(
    CommandStoreClient* commandStoreClient,
    FrontendEvents* events,
    ConfirmDialog* confirmDialog,
    SnippetImportDialog* snippetImportDialog,
    std::function<void(std::string const&, bool)> runInTerminal,
    Nui::Observed<bool>* connectionLost
)
    : impl_{std::make_unique<Implementation>(
          commandStoreClient, events, confirmDialog, snippetImportDialog, std::move(runInTerminal), connectionLost
      )}
{
    // A null client means the backend store failed to open; the panel then only shows a notice.
    if (impl_->client)
    {
        auto* const implementation = impl_.get();
        const auto refresh = [implementation](auto const&) {
            implementation->refreshCards();
            Nui::globalEventContext.executeActiveEventsImmediately();
        };
        impl_->snippetsListener = Nui::smartListen(impl_->client->snippets(), [implementation](auto const&) {
            implementation->rebuildTagPills();
            implementation->refreshCards();
            Nui::globalEventContext.executeActiveEventsImmediately();
        });
        // Folders decide which snippets count as unfiled.
        impl_->foldersListener = Nui::smartListen(impl_->client->folders(), refresh);
        impl_->searchListener = Nui::smartListen(impl_->searchQuery, refresh);
        impl_->folderSelectionListener = Nui::smartListen(impl_->folderSelection, refresh);
        impl_->tagsListener = Nui::smartListen(impl_->selectedTags, refresh);
    }
}
CommandSnippetsPanel::~CommandSnippetsPanel() = default;
CommandSnippetsPanel::CommandSnippetsPanel(CommandSnippetsPanel&&) = default;
CommandSnippetsPanel& CommandSnippetsPanel::operator=(CommandSnippetsPanel&&) = default;

Nui::ElementRenderer CommandSnippetsPanel::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    if (!impl_->client)
    {
        return div{class_ = "command-snippets-panel"}(
            div{class_ = "cmds-empty"}(language->get("commandSnippetsPanel", "storeUnavailable"))
        );
    }

    impl_->client->reloadSnippets();
    impl_->client->reloadFolders();
    impl_->refreshCards();

    // clang-format off
    return div{class_ = "command-snippets-panel"}(
        // Focusable, so a click on a row (rows are regenerated) leaves the focus here for F2.
        div{
            class_ = "cmds-sidebar-host",
            tabIndex = "-1",
            onKeyDown = [this](Nui::val event) {
                if (event["key"].as<std::string>() != "F2")
                    return;
                auto const& selection = impl_->folderSelection.value();
                if (!selection.starts_with("folder:"))
                    return;
                event.call<void>("preventDefault");
                impl_->startRename(selection.substr(7));
            },
        }(
            Nui::observe(impl_->client->snippets(), impl_->client->folders(), impl_->folderSelection,
                         impl_->newFolderInputVisible, impl_->renamingFolderId)
                .generate([this]() -> Nui::ElementRenderer {
                    return impl_->renderSidebar();
                })
        ),
        div{class_ = "cmds-main"}(
            div{class_ = "cmds-toolbar"}(
                div{class_ = "cmds-search-box"}(
                    Ui5Icons::search(),
                    input{
                        type = "text",
                        class_ = "cmds-search",
                        placeHolder = language->get("commandSnippetsPanel", "searchPlaceholder"),
                        onInput = [this](Nui::val event) {
                            impl_->searchQuery = event["target"]["value"].as<std::string>();
                        },
                    }()
                ),
                button{
                    class_ = "cmds-button",
                    title = language->get("commandSnippetsPanel", "copySnippetsTooltip"),
                    onClick = [this](Nui::val) {
                        impl_->copyVisibleSnippets();
                    },
                }(Ui5Icons::copy(), span{}(language->get("commandSnippetsPanel", "copySnippets"))),
                button{
                    class_ = "cmds-button",
                    title = language->get("commandSnippetsPanel", "importSnippetsTooltip"),
                    onClick = [this](Nui::val) {
                        impl_->openImport();
                    },
                }(Ui5Icons::paste(), span{}(language->get("commandSnippetsPanel", "importSnippets"))),
                button{
                    class_ = "cmds-button cmds-button-primary",
                    title = language->get("commandSnippetsPanel", "newSnippetTooltip"),
                    onClick = [this](Nui::val) {
                        auto fresh = CommandStore::Snippet{};
                        // A snippet created from a folder view starts in that folder.
                        auto const& selection = impl_->folderSelection.value();
                        if (selection.starts_with("folder:"))
                            fresh.folder = selection.substr(7);
                        impl_->openEditor(fresh);
                    },
                }(Ui5Icons::add(), span{}(language->get("commandSnippetsPanel", "newSnippet")))
            ),
            ScriptNuiComponents::pillList({
                .pills = impl_->tagPills,
            }),
            div{class_ = "cmds-grid-host"}(
                div{class_ = "cmds-empty-host"}(
                    Nui::observe(impl_->cards, impl_->client->snippets()).generate([this]() -> Nui::ElementRenderer {
                        if (!impl_->cards.value().empty())
                            return Nui::nil();
                        const bool noSnippets = impl_->client->snippets().value().empty();
                        return div{class_ = "cmds-empty"}(
                            language->get("commandSnippetsPanel", noSnippets ? "emptyText" : "noMatchesText")
                        );
                    })
                ),
                // Bound to the observed cards: a change redraws only the cards it touched.
                div{class_ = "cmds-grid"}(
                    Nui::range(impl_->cards),
                    [this](long long, SnippetCard const& card) -> Nui::ElementRenderer {
                        return impl_->renderCard(card);
                    }
                )
            )
        ),
        div{class_ = "cmds-editor-host"}(
            Nui::observe(impl_->editorVisible).generate([this]() -> Nui::ElementRenderer {
                return impl_->renderEditor();
            })
        ),
        div{class_ = "cmds-variable-form-host"}(
            Nui::observe(impl_->variableFormVisible).generate([this]() -> Nui::ElementRenderer {
                return impl_->renderVariableForm();
            })
        )
    );
    // clang-format on
}
