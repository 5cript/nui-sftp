#include <frontend/dialog/snippet_import_dialog.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <frontend/command_store/command_store_client.hpp>
#include <frontend/command_store/danger_level_text.hpp>
#include <frontend/components/code_editor.hpp>
#include <frontend/notifications.hpp>

#include <command-store/snippet_transfer.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/select.hpp>
#include <ui5-sap-icons/icons/paste.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/event_system/listen.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using CommandStore::SnippetImport::ImportPlan;
    using CommandStore::SnippetImport::PlannedSnippet;
    using CommandStore::SnippetImport::Resolution;
    using CommandStore::SnippetImport::Status;
    using CommandStore::SnippetImport::Target;
    using CommandStore::SnippetTransfer::ParseError;
    using CommandStore::SnippetTransfer::ParseFailure;
    using Button = ScriptNuiComponents::Dialog::Button;

    constexpr std::string_view choiceDocument = "document";
    constexpr std::string_view choiceUnfiled = "unfiled";
    constexpr std::string_view choiceFolderPrefix = "folder:";

    enum class Step
    {
        Paste,
        Review,
    };

    std::string parseFailureMessage(ParseFailure const& failure)
    {
        const auto entryNumber = failure.entryIndex + 1;
        switch (failure.error)
        {
            case ParseError::InvalidJson:
                return language->get("snippetImportDialog", "invalidJson");
            case ParseError::UnsupportedDocument:
                return language->get("snippetImportDialog", "unsupportedDocument");
            case ParseError::UnsupportedVersion:
                return fmt::format(
                    fmt::runtime(language->get("snippetImportDialog", "unsupportedVersion")), failure.detail
                );
            case ParseError::EntryWithoutName:
                return fmt::format(fmt::runtime(language->get("snippetImportDialog", "entryWithoutName")), entryNumber);
            case ParseError::EntryWithoutCommand:
                return fmt::format(
                    fmt::runtime(language->get("snippetImportDialog", "entryWithoutCommand")), entryNumber
                );
        }
        return language->get("snippetImportDialog", "invalidJson");
    }

    std::string folderLabel(std::string const& folderName)
    {
        return folderName.empty() ? language->get("snippetImportDialog", "unfiled") : folderName;
    }

    /**
     * @brief Identifies a conflict across edits of the pasted text, so a choice survives retyping.
     */
    std::string resolutionKey(PlannedSnippet const& planned)
    {
        return fmt::format(
            "{}\n{}\n{}\n{}\n{}",
            planned.snippet.folder,
            planned.snippet.name,
            planned.snippet.command,
            planned.snippet.tags,
            CommandStore::toString(planned.snippet.danger)
        );
    }

    Nui::ElementRenderer shownIf(bool condition, Nui::ElementRenderer renderer)
    {
        return condition ? std::move(renderer) : Nui::nil();
    }

    Nui::ElementRenderer listOf(std::string const& className, std::vector<Nui::ElementRenderer> items)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        return div{
            class_ = className
        }(Nui::range(std::move(items)),
            [](long long, auto const& item) -> Nui::ElementRenderer
            {
                return item;
            });
    }

    /**
     * @brief A command on one line, or a scrollable read-only box when it spans several lines.
     */
    Nui::ElementRenderer commandView(std::string const& command)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;

        if (command.find('\n') != std::string::npos)
            return pre{class_ = "snippet-import-dialog-multiline", tabIndex = "0"}(command);
        return span{class_ = "snippet-import-dialog-code"}(command);
    }

    Nui::ElementRenderer tagsView(std::vector<std::string> const& tags)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;

        if (tags.empty())
            return span{class_ = "snippet-import-dialog-no-tags"}(language->get("snippetImportDialog", "noTags"));
        std::vector<Nui::ElementRenderer> pills{};
        pills.reserve(tags.size());
        for (auto const& tag : tags)
            pills.push_back(span{class_ = "snippet-import-dialog-tag"}(tag));
        return listOf("snippet-import-dialog-tag-list", std::move(pills));
    }

    Nui::ElementRenderer dangerView(std::optional<CommandStore::DangerLevel> level)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;

        return span{
            class_ = "snippet-import-dialog-danger",
            "data-danger"_attr = std::string{CommandStore::toString(level)},
        }(dangerLevelText(level));
    }
}

struct SnippetImportDialog::Implementation
{
    std::string id;
    CommandStoreClient* client;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog{};

    Target target{};
    SnippetImportDialog::Source source{SnippetImportDialog::Source::Pasted};
    /** @brief Target choices of the select, (label, choice id); observed, the folders change. */
    Nui::Observed<std::vector<std::pair<std::string, std::string>>> targetChoices{};
    Nui::Observed<std::pair<std::string, std::string>> targetChoice{};

    Nui::Observed<Step> step{Step::Paste};

    /** @brief The pasted text. */
    std::string draft{};
    std::optional<Components::CodeEditor> editor{};
    Nui::Observed<int> editorGeneration{0};

    std::optional<ParseFailure> failure{};
    std::optional<ImportPlan> plan{};
    std::map<std::string, Resolution> resolutions{};
    /** @brief Bumped when the plan changed; redraws the review lists. */
    Nui::Observed<int> planRevision{0};
    /** @brief Bumped when a resolution changed; redraws the tally only, so the lists keep their scroll. */
    Nui::Observed<int> decisionRevision{0};
    /** @brief An import RPC is pending; every button stays disabled until it answers. */
    bool importing{false};
    bool isOpen{false};
    Nui::ListenRemover<Nui::Observed<std::vector<CommandStore::SnippetFolder>>> foldersListener{};
    Nui::ListenRemover<Nui::Observed<std::vector<CommandStore::Snippet>>> snippetsListener{};

    Implementation(std::string id, CommandStoreClient* client)
        : id{std::move(id)}
        , client{client}
    {}

    /**
     * @brief Replans against the changed store; a target folder that vanished falls back to the
     *        folders the document names and sends the user back to choose.
     */
    void onStoreChanged()
    {
        if (!isOpen || importing)
            return;
        if (target.folderId && !target.folderId->empty())
        {
            auto const& folders = client->folders().value();
            const auto stillExists = std::ranges::any_of(
                folders,
                [this](CommandStore::SnippetFolder const& folder)
                {
                    return folder.id == *target.folderId;
                }
            );
            if (!stillExists)
            {
                target = Target{};
                Notifications::warning(language->get("snippetImportDialog", "targetFolderGone"));
                step = Step::Paste;
            }
        }
        rebuildTargetChoices();
        replan();
    }

    void rebuildTargetChoices()
    {
        std::vector<std::pair<std::string, std::string>> choices{};
        choices.emplace_back(language->get("snippetImportDialog", "targetDocument"), choiceDocument);
        choices.emplace_back(language->get("snippetImportDialog", "unfiled"), choiceUnfiled);
        for (auto const& folder : client->folders().value())
        {
            choices.emplace_back(
                fmt::format(fmt::runtime(language->get("snippetImportDialog", "targetFolder")), folder.name),
                fmt::format("{}{}", choiceFolderPrefix, folder.id)
            );
        }

        const auto selected = !target.folderId ? std::string{choiceDocument}
            : target.folderId->empty()         ? std::string{choiceUnfiled}
                                               : fmt::format("{}{}", choiceFolderPrefix, *target.folderId);
        const auto chosen = std::ranges::find_if(
            choices,
            [&selected](auto const& choice)
            {
                return choice.second == selected;
            }
        );
        targetChoice = chosen != choices.end() ? *chosen : choices.front();
        targetChoices = std::move(choices);
    }

    void chooseTarget(std::string const& choice)
    {
        if (choice == choiceDocument)
            target = Target{};
        else if (choice == choiceUnfiled)
            target = Target{.folderId = std::string{}};
        else
            target = Target{.folderId = choice.substr(choiceFolderPrefix.size())};
        replan();
    }

    std::string targetFolderName() const
    {
        auto const& folders = client->folders().value();
        const auto folder = std::ranges::find_if(
            folders,
            [this](CommandStore::SnippetFolder const& candidate)
            {
                return candidate.id == *target.folderId;
            }
        );
        return folder != folders.end() ? folder->name : std::string{};
    }

    void replan()
    {
        failure.reset();
        plan.reset();
        if (draft.find_first_not_of(" \t\r\n") != std::string::npos)
        {
            auto parsed = CommandStore::SnippetTransfer::parse(draft);
            if (parsed)
                plan = CommandStore::SnippetImport::plan(
                    std::move(*parsed), client->snippets().value(), client->folders().value(), target
                );
            else
                failure = parsed.error();
        }
        planRevision = planRevision.value() + 1;
        decisionRevision = decisionRevision.value() + 1;
        updateButtons();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    Resolution resolutionOf(std::size_t index) const
    {
        const auto chosen = resolutions.find(resolutionKey(plan->snippets[index]));
        return chosen == resolutions.end() ? Resolution::Skip : chosen->second;
    }

    void choose(std::size_t index, Resolution resolution)
    {
        resolutions[resolutionKey(plan->snippets[index])] = resolution;
        decisionRevision = decisionRevision.value() + 1;
        updateButtons();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    /**
     * @brief Replace only applies where a stored snippet can be replaced; the other conflicts keep
     *        their choice.
     */
    void applyToAll(Resolution resolution)
    {
        if (!plan)
            return;
        for (auto const& planned : plan->snippets)
        {
            if (planned.status == Status::NameConflict && (resolution != Resolution::Replace || planned.canReplace()))
                resolutions[resolutionKey(planned)] = resolution;
        }
        planRevision = planRevision.value() + 1;
        decisionRevision = decisionRevision.value() + 1;
        updateButtons();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    CommandStore::SnippetImport::Decision decision() const
    {
        if (!plan)
            return {};
        return CommandStore::SnippetImport::decide(
            *plan,
            [this](std::size_t index)
            {
                return resolutionOf(index);
            }
        );
    }

    void updateButtons()
    {
        if (importing)
        {
            dialog->setButtonsDisabled(Button::Ok | Button::No | Button::Cancel, true);
            return;
        }
        dialog->setButtonsDisabled(Button::Cancel, false);

        auto labels = localizedButtonLabels();
        labels.no = language->get("snippetImportDialog", "backButton");
        if (step.value() == Step::Paste)
        {
            labels.ok = language->get("snippetImportDialog", "nextButton");
            dialog->setButtonLabels(labels);
            dialog->setButtonsDisabled(Button::Ok, !plan || plan->snippets.empty());
            dialog->setButtonsDisabled(Button::No, true);
            return;
        }

        // Entries that only merge a favorite flag are worth importing too, but are no snippets to count.
        const auto decided = decision();
        const auto stored = decided.added + decided.replaced;
        if (stored == 0 && !decided.entries.empty())
            labels.ok = language->get("snippetImportDialog", "importButtonFavorites");
        else
            labels.ok = stored == 1
                ? language->get("snippetImportDialog", "importButtonOne")
                : fmt::format(fmt::runtime(language->get("snippetImportDialog", "importButtonMany")), stored);
        dialog->setButtonLabels(labels);
        dialog->setButtonsDisabled(Button::Ok, decided.entries.empty());
        dialog->setButtonsDisabled(Button::No, false);
    }

    void goTo(Step next)
    {
        step = next;
        // The review is only built while shown, so typing in the editor does not rebuild it.
        if (next == Step::Review)
        {
            planRevision = planRevision.value() + 1;
            decisionRevision = decisionRevision.value() + 1;
        }
        updateButtons();
        Nui::globalEventContext.executeActiveEventsImmediately();
        if (next == Step::Paste && editor)
            editor->focus();
    }

    /**
     * @brief Next and Back move between the steps; Import closes the dialog once it succeeded.
     */
    bool onButton(Button button)
    {
        if (button == Button::Ok && step.value() == Step::Paste)
        {
            goTo(Step::Review);
            return false;
        }
        if (button == Button::Ok)
        {
            import();
            return false;
        }
        if (button == Button::No)
        {
            goTo(Step::Paste);
            return false;
        }
        return true;
    }

    void pasteFromClipboard()
    {
        Nui::val::global("navigator")["clipboard"]
            .call<Nui::val>("readText")
            .call<void>(
                "then",
                Nui::bind(
                    [this](Nui::val clipboardText)
                    {
                        // WebKit hands out text from other applications only with paste access.
                        const auto pasted = clipboardText.as<std::string>();
                        if (pasted.empty())
                            return Notifications::warning(language->get("snippetImportDialog", "clipboardEmpty"));
                        if (editor)
                            editor->setValue(pasted);
                    },
                    std::placeholders::_1
                ),
                Nui::bind(
                    [](Nui::val error)
                    {
                        Notifications::error(
                            fmt::format(
                                fmt::runtime(language->get("snippetImportDialog", "pasteFailed")),
                                error.call<std::string>("toString")
                            )
                        );
                    },
                    std::placeholders::_1
                )
            );
    }

    void import()
    {
        auto decided = decision();
        if (decided.entries.empty())
            return;

        importing = true;
        updateButtons();
        Nui::globalEventContext.executeActiveEventsImmediately();
        client->importSnippets(
            std::move(decided.entries),
            [this, added = decided.added, replaced = decided.replaced, skipped = decided.skipped](
                CommandStore::ImportSummary const& summary
            )
            {
                importing = false;
                dialog->close();
                // The decision's counts leave out entries that only merged a favorite flag.
                Notifications::success(
                    fmt::format(
                        fmt::runtime(language->get("snippetImportDialog", "importDone")),
                        added,
                        replaced,
                        skipped,
                        summary.foldersCreated
                    )
                );
            },
            [this]()
            {
                // The paste and the decisions stay, so the user can retry or cancel.
                importing = false;
                updateButtons();
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        );
    }

    void closeEditor()
    {
        isOpen = false;
        editor.reset();
        editorGeneration = editorGeneration.value() + 1;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    Nui::Attribute visibleOn(Step shownStep)
    {
        using namespace Nui::Attributes;
        return style = Nui::observe(step).generate(
                   [this, shownStep]()
                   {
                       return step.value() == shownStep ? std::string{} : std::string{"display: none"};
                   }
               );
    }

    Nui::ElementRenderer pasteStatus();
    Nui::ElementRenderer pasteStep();
    Nui::ElementRenderer reviewIntro();
    Nui::ElementRenderer conflictCard(std::size_t index);
    Nui::ElementRenderer reviewSections();
    Nui::ElementRenderer tally();
    Nui::ElementRenderer reviewStep();
    Nui::ElementRenderer body();
};

Nui::ElementRenderer SnippetImportDialog::Implementation::pasteStatus()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    if (failure)
        return div{class_ = "snippet-import-dialog-status error"}(parseFailureMessage(*failure));
    if (!plan)
        return div{class_ = "snippet-import-dialog-status"}(language->get("snippetImportDialog", "emptyHint"));
    if (plan->snippets.empty())
        return div{class_ = "snippet-import-dialog-status error"}(language->get("snippetImportDialog", "noSnippets"));

    const auto count = plan->snippets.size();
    return div{
        class_ = "snippet-import-dialog-status valid"
    }(count == 1 ? language->get("snippetImportDialog", "foundOne")
                 : fmt::format(fmt::runtime(language->get("snippetImportDialog", "foundMany")), count));
}

Nui::ElementRenderer SnippetImportDialog::Implementation::pasteStep()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::label;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    // clang-format off
    return div{class_ = "snippet-import-dialog-step", visibleOn(Step::Paste)}(
        div{class_ = "snippet-import-dialog-step-name"}(language->get("snippetImportDialog", "stepPaste")),
        div{class_ = "snippet-import-dialog-target"}(
            label{class_ = "snippet-import-dialog-label"}(language->get("snippetImportDialog", "targetLabel")),
            ScriptNuiComponents::select(
                ScriptNuiComponents::SelectOptions<decltype(targetChoice), decltype(targetChoices)>{
                    .activeOption = targetChoice,
                    .options = targetChoices,
                    .onChange =
                        [this](std::pair<std::string, std::string> const& choice, Nui::WebApi::MouseEvent const&) {
                            chooseTarget(choice.second);
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
            span{}(),
            div{class_ = "snippet-import-dialog-hint"}(language->get("snippetImportDialog", "targetHint"))
        ),
        div{class_ = "snippet-import-dialog-json-header"}(
            label{class_ = "snippet-import-dialog-label"}(language->get("snippetImportDialog", "jsonLabel")),
            ScriptNuiComponents::button({
                .text = language->get("snippetImportDialog", "pasteFromClipboard"),
                .icon = Ui5Icons::paste(),
                .attributes = {
                    title = language->get("snippetImportDialog", "pasteFromClipboardTooltip"),
                    onClick = [this](Nui::val) {
                        pasteFromClipboard();
                    },
                },
            })
        ),
        div{class_ = "snippet-import-dialog-editor"}(
            Nui::observe(editorGeneration).generate([this]() -> Nui::ElementRenderer {
                if (!editor)
                    return Nui::nil();
                return (*editor)();
            })
        ),
        div{class_ = "snippet-import-dialog-status-host"}(
            Nui::observe(planRevision).generate([this]() -> Nui::ElementRenderer {
                return pasteStatus();
            })
        )
    );
    // clang-format on
}

Nui::ElementRenderer SnippetImportDialog::Implementation::reviewIntro()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    if (!plan || step.value() != Step::Review)
        return Nui::nil();

    const auto count = plan->snippets.size();
    const auto one = count == 1;
    std::string intro{};
    if (!target.folderId)
    {
        intro = one ? language->get("snippetImportDialog", "introDocumentOne")
                    : fmt::format(fmt::runtime(language->get("snippetImportDialog", "introDocumentMany")), count);
        if (!plan->newFolders.empty())
        {
            intro += " ";
            intro += fmt::format(
                fmt::runtime(language->get("snippetImportDialog", "introNewFolders")), fmt::join(plan->newFolders, ", ")
            );
        }
    }
    else if (target.folderId->empty())
    {
        intro = one ? language->get("snippetImportDialog", "introUnfiledOne")
                    : fmt::format(fmt::runtime(language->get("snippetImportDialog", "introUnfiledMany")), count);
    }
    else
    {
        intro = one
            ? fmt::format(fmt::runtime(language->get("snippetImportDialog", "introFolderOne")), targetFolderName())
            : fmt::format(
                  fmt::runtime(language->get("snippetImportDialog", "introFolderMany")), count, targetFolderName()
              );
    }
    return div{class_ = "snippet-import-dialog-intro"}(intro);
}

Nui::ElementRenderer SnippetImportDialog::Implementation::conflictCard(std::size_t index)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::label;
    using Nui::Elements::span;

    auto const& planned = plan->snippets[index];
    const auto chosen = resolutionOf(index);
    const auto group = fmt::format("{}-conflict-{}", id, index);

    const auto choice = [this, index, chosen, &group](Resolution resolution, std::string const& text)
    {
        return label{class_ = "snippet-import-dialog-choice"}(
            input{
                type = "radio",
                name = group,
                "checked"_prop = resolution == chosen,
                onChange =
                    [this, index, resolution](Nui::val)
                {
                    choose(index, resolution);
                },
            }(),
            span{}(text)
        );
    };

    auto const* earlier = planned.existing ? nullptr : &plan->snippets[*planned.earlierEntry].snippet;
    auto const& otherCommand = earlier ? earlier->command : planned.existing->command;
    auto const& otherTags = earlier ? earlier->tags : planned.existing->tags;
    const auto otherDanger = earlier ? earlier->danger : planned.existing->danger;

    const auto otherTitle = language->get("snippetImportDialog", planned.existing ? "existing" : "earlier");
    const auto pastedTitle =
        language->get("snippetImportDialog", source == SnippetImportDialog::Source::Preset ? "preset" : "pasted");
    // Only what differs is compared; a conflict differs in the command, the tags, the danger level or several.
    const auto sides = [&](std::string const& caption, Nui::ElementRenderer other, Nui::ElementRenderer pasted)
    {
        return div{
            class_ = "snippet-import-dialog-comparison"
        }(div{class_ = "snippet-import-dialog-comparison-caption"}(caption),
            div{class_ = "snippet-import-dialog-sides"}(
                div{
                    class_ = "snippet-import-dialog-side"
                }(div{class_ = "snippet-import-dialog-side-label"}(otherTitle), std::move(other)),
                div{
                    class_ = "snippet-import-dialog-side"
                }(div{class_ = "snippet-import-dialog-side-label"}(pastedTitle), std::move(pasted))
            ));
    };

    // clang-format off
    return div{class_ = "snippet-import-dialog-conflict"}(
        div{class_ = "snippet-import-dialog-conflict-head"}(
            span{class_ = "snippet-import-dialog-conflict-name"}(planned.snippet.name),
            shownIf(!target.folderId, span{class_ = "snippet-import-dialog-conflict-folder"}(folderLabel(planned.snippet.folder))),
            div{class_ = "snippet-import-dialog-choices"}(
                choice(Resolution::Skip, language->get("snippetImportDialog", "resolutionSkip")),
                shownIf(planned.canReplace(), choice(Resolution::Replace, language->get("snippetImportDialog", "resolutionReplace"))),
                choice(Resolution::KeepBoth, language->get("snippetImportDialog", "resolutionKeepBoth"))
            )
        ),
        shownIf(otherCommand != planned.snippet.command, sides(
            language->get("snippetImportDialog", "commandCaption"), commandView(otherCommand), commandView(planned.snippet.command)
        )),
        shownIf(otherTags != planned.snippet.tags, sides(
            language->get("snippetImportDialog", "tagsCaption"), tagsView(otherTags), tagsView(planned.snippet.tags)
        )),
        shownIf(otherDanger != planned.snippet.danger, sides(
            language->get("snippetImportDialog", "dangerCaption"), dangerView(otherDanger), dangerView(planned.snippet.danger)
        ))
    );
    // clang-format on
}

Nui::ElementRenderer SnippetImportDialog::Implementation::reviewSections()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    if (!plan || step.value() != Step::Review)
        return Nui::nil();

    const auto byDocument = !target.folderId;
    const auto rowClass =
        byDocument ? std::string{"snippet-import-dialog-row with-folder"} : std::string{"snippet-import-dialog-row"};

    std::vector<Nui::ElementRenderer> conflicts{};
    std::vector<Nui::ElementRenderer> rows{};
    rows.push_back(
        div{
            class_ = rowClass + " head"
        }(span{}(language->get("snippetImportDialog", "columnName")),
            shownIf(byDocument, span{}(language->get("snippetImportDialog", "columnFolder"))),
            span{}(language->get("snippetImportDialog", "columnCommand")),
            span{}(language->get("snippetImportDialog", "columnResult")))
    );
    for (std::size_t index = 0; index < plan->snippets.size(); ++index)
    {
        auto const& planned = plan->snippets[index];
        if (planned.status == Status::NameConflict)
        {
            conflicts.push_back(conflictCard(index));
            continue;
        }
        const auto identical = planned.status == Status::Identical;
        rows.push_back(
            div{
                class_ = identical ? rowClass + " identical" : rowClass
            }(span{}(planned.snippet.name),
                shownIf(byDocument, span{}(folderLabel(planned.snippet.folder))),
                commandView(planned.snippet.command),
                span{}(language->get("snippetImportDialog", identical ? "resultIdentical" : "resultAdded")))
        );
    }
    const auto hasConflicts = !conflicts.empty();
    const auto hasRows = rows.size() > 1;

    const auto applyButton = [this](Resolution resolution, std::string const& text)
    {
        return button{
            class_ = "snippet-import-dialog-apply",
            type = "button",
            onClick = [this, resolution](Nui::val)
            {
                applyToAll(resolution);
            },
        }(text);
    };

    // clang-format off
    return div{class_ = "snippet-import-dialog-sections"}(
        shownIf(hasConflicts, section{class_ = "snippet-import-dialog-conflicts"}(
            div{class_ = "snippet-import-dialog-conflicts-head"}(
                span{class_ = "snippet-import-dialog-section-title"}(language->get("snippetImportDialog", "conflicts")),
                div{class_ = "snippet-import-dialog-apply-all"}(
                    span{}(language->get("snippetImportDialog", "applyToAll")),
                    applyButton(Resolution::Skip, language->get("snippetImportDialog", "resolutionSkip")),
                    applyButton(Resolution::Replace, language->get("snippetImportDialog", "resolutionReplace")),
                    applyButton(Resolution::KeepBoth, language->get("snippetImportDialog", "resolutionKeepBoth"))
                )
            ),
            listOf("snippet-import-dialog-conflict-list", std::move(conflicts))
        )),
        shownIf(hasRows, section{class_ = "snippet-import-dialog-results"}(
            span{class_ = "snippet-import-dialog-section-title"}(language->get("snippetImportDialog", "noConflicts")),
            listOf("snippet-import-dialog-table", std::move(rows))
        ))
    );
    // clang-format on
}

Nui::ElementRenderer SnippetImportDialog::Implementation::tally()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    if (step.value() != Step::Review)
        return Nui::nil();

    const auto decided = decision();
    return div{class_ = "snippet-import-dialog-tally"}(fmt::format(
        fmt::runtime(language->get("snippetImportDialog", "tally")), decided.added, decided.replaced, decided.skipped
    ));
}

Nui::ElementRenderer SnippetImportDialog::Implementation::reviewStep()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{class_ = "snippet-import-dialog-step", visibleOn(Step::Review)}(
        div{class_ = "snippet-import-dialog-step-name"}(language->get("snippetImportDialog", "stepReview")),
        div{class_ = "snippet-import-dialog-intro-host"}(
            Nui::observe(planRevision).generate([this]() -> Nui::ElementRenderer {
                return reviewIntro();
            })
        ),
        div{class_ = "snippet-import-dialog-sections-host"}(
            Nui::observe(planRevision).generate([this]() -> Nui::ElementRenderer {
                return reviewSections();
            })
        ),
        div{class_ = "snippet-import-dialog-tally-host"}(
            Nui::observe(decisionRevision).generate([this]() -> Nui::ElementRenderer {
                return tally();
            })
        )
    );
    // clang-format on
}

Nui::ElementRenderer SnippetImportDialog::Implementation::body()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;

    return section{class_ = "snippet-import-dialog"}(pasteStep(), reviewStep());
}

SnippetImportDialog::SnippetImportDialog(std::string id, CommandStoreClient* client)
    : impl_{std::make_unique<Implementation>(std::move(id), client)}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, impl_->body());
    impl_->foldersListener = Nui::smartListen(client->folders(), [implementation = impl_.get()](auto const&) {
        implementation->onStoreChanged();
    });
    impl_->snippetsListener = Nui::smartListen(client->snippets(), [implementation = impl_.get()](auto const&) {
        implementation->onStoreChanged();
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SnippetImportDialog);

Nui::ElementRenderer SnippetImportDialog::operator()()
{
    return (*impl_->dialog)();
}

void SnippetImportDialog::open(OpenOptions options)
{
    impl_->target = std::move(options.target);
    impl_->source = options.source;
    impl_->draft = options.text;
    impl_->resolutions.clear();
    impl_->step = Step::Paste;
    impl_->isOpen = true;
    impl_->rebuildTargetChoices();
    impl_->editor.emplace(
        Components::CodeEditor::Settings{
            .language = "json",
            .initialValue = std::move(options.text),
            .onChange = [this](std::string const& changed)
            {
                impl_->draft = changed;
                impl_->replan();
            },
        }
    );
    impl_->editorGeneration = impl_->editorGeneration.value() + 1;

    impl_->dialog->open({
        .headerText = language->get("snippetImportDialog", "title"),
        .buttons = Button::Ok | Button::No | Button::Cancel,
        .disabledButtons = Button::Ok | Button::No,
        .onClose =
            [this](std::optional<Button>)
        {
            impl_->closeEditor();
        },
        .onButton =
            [this](Button button)
        {
            return impl_->onButton(button);
        },
        .modal = true,
        .mayCloseWithoutButton = false,
    });
    impl_->replan();
    if (options.startAtReview && impl_->plan && !impl_->plan->snippets.empty())
        impl_->goTo(Step::Review);
    else
        impl_->editor->focus();
}
