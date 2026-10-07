#include <frontend/settings/search/settings_search.hpp>
#include <frontend/icon_from_name.hpp>
#include <frontend/session_components/command_panel_helpers.hpp>
#include <frontend/svgs/it-system.hpp>
#include <frontend/svgs/settings.hpp>
#include <persistence/state/state.hpp>
#include <utility/fuzzy_search.hpp>
#include <utility/language.hpp>

#include <ui5-sap-icons/icons/nav-back.hpp>
#include <ui5-sap-icons/icons/search.hpp>
#include <ui5-sap-icons/icons/slim-arrow-right.hpp>

#include <nui/event_system/listen.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/val.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

using namespace std::string_literals;

namespace
{
    constexpr std::size_t maximumResults = 10u;
    constexpr std::size_t maximumValueLength = 40u;

    std::string translated(std::vector<std::string> const& key)
    {
        return language->findByPathWithFallback(language->currentLanguage(), key)
            .value_or(key.empty() ? std::string{} : key.back());
    }

    std::string searchText(char const* key)
    {
        return language->get("settings", "search", key);
    }

    nlohmann::json const* findPath(nlohmann::json const& object, std::span<std::string const> path)
    {
        auto const* current = &object;
        for (auto const& segment : path)
        {
            if (!current->is_object())
                return nullptr;
            const auto iterator = current->find(segment);
            if (iterator == current->end())
                return nullptr;
            current = &*iterator;
        }
        return current;
    }

    nlohmann::json const* findPath(nlohmann::json const& object, std::initializer_list<std::string> path)
    {
        return findPath(object, std::span<std::string const>{path.begin(), path.size()});
    }

    bool isInheritableGroup(std::string const& group)
    {
        return group == "sshOptions" || group == "sftpOptions" || group == "terminalOptions" || group == "termios" ||
            group == "queueOptions" || group == "historyOptions";
    }

    /**
     * @brief The object of a session that holds its own values of an inheritable group, next to "$ref".
     */
    nlohmann::json const* inlineGroup(nlohmann::json const& session, std::string const& group)
    {
        if (group == "sshOptions" || group == "sftpOptions")
            return findPath(session, {"engine", group});
        return findPath(session, {group});
    }

    /**
     * @brief The value a session itself holds for a setting, if the setting path can be found in the session.
     */
    nlohmann::json const* sessionValue(nlohmann::json const& session, SettingKeyPath const& path)
    {
        if (path.empty())
            return nullptr;
        if (isInheritableGroup(path.front()))
        {
            auto const* group = inlineGroup(session, path.front());
            if (!group)
                return nullptr;
            return findPath(*group, std::span<std::string const>{path}.subspan(1));
        }
        if (path.front() != "sessionOptions")
            return nullptr;
        if (auto const* engineValue = findPath(session, {"engine", path.back()}))
            return engineValue;
        if (path.size() == 2u)
            return findPath(session, {path.back()});
        return nullptr;
    }

    /**
     * @brief Whether the persisted state uses the setting path anywhere. A path that appears nowhere may just be
     * named differently in the state, so no status is shown for it rather than a wrong one.
     */
    bool isPathKnown(nlohmann::json const& state, SettingKeyPath const& path)
    {
        if (path.empty())
            return false;
        auto const* sessions = findPath(state, {"sessions"});
        if (sessions && sessions->is_object())
        {
            for (auto const& session : sessions->items())
            {
                if (sessionValue(session.value(), path))
                    return true;
            }
        }
        if (!isInheritableGroup(path.front()))
            return false;
        auto const* groups = findPath(state, {path.front()});
        if (!groups || !groups->is_object())
            return false;
        for (auto const& group : groups->items())
        {
            if (findPath(group.value(), std::span<std::string const>{path}.subspan(1)))
                return true;
        }
        return false;
    }

    std::string displayValue(nlohmann::json const& value)
    {
        auto text = value.is_string() ? value.get<std::string>() : value.dump();
        if (text.size() > maximumValueLength)
            text = text.substr(0, maximumValueLength) + "...";
        return text;
    }

    /**
     * @brief The scope a result is described by: the inheritable defaults when there are any, else where it is.
     */
    std::optional<SettingScope> primaryScope(SettingsSearchIndex::Entry const& entry)
    {
        for (const auto scope : {SettingScope::Inheritable, SettingScope::Session, SettingScope::General})
        {
            if (entry.renderedScope(scope))
                return scope;
        }
        return std::nullopt;
    }

    Nui::ElementRenderer passThrough(long long, Nui::ElementRenderer const& renderer)
    {
        return renderer;
    }

    bool needsTargetChoice(SettingsSearchIndex::Entry const& entry)
    {
        return entry.renderedScope(SettingScope::Inheritable) || entry.renderedScope(SettingScope::Session);
    }
}

struct SettingsSearch::Implementation
{
    /**
     * @brief One place a setting can be shown at, on the second level of the results.
     */
    struct TargetRow
    {
        Target target{};
        std::string name{};
        std::string iconName{};
        std::string note{};
        std::string status{};
    };

    SettingsSearchIndex* index;
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    std::function<std::optional<std::string>()> openSession;
    NavigateFunction navigate;

    Nui::Observed<bool> paneOpen{false};
    Nui::Observed<int> revision{0};
    Nui::Observed<std::size_t> selection{0u};

    std::string query{};
    std::vector<SettingsSearchIndex::SearchResult> results{};

    /**
     * @brief The result whose targets are listed, while on the second level.
     */
    std::optional<std::size_t> drilledResult{};
    std::string serverFilter{};
    std::vector<TargetRow> pinnedTargets{};
    std::vector<TargetRow> allServerTargets{};
    std::vector<TargetRow> listedTargets{};

    Nui::ListenRemover<decltype(FrontendEvents::settingsOpen)> settingsOpenListener{};

    Nui::val rootElement{};
    Nui::val inputElement{};
    Nui::val documentMouseDown{};
    Nui::val documentKeyDown{};

    Implementation(
        SettingsSearchIndex& index,
        Persistence::StateHolder& stateHolder,
        FrontendEvents& events,
        std::function<std::optional<std::string>()> openSession,
        NavigateFunction navigate
    )
        : index{&index}
        , stateHolder{&stateHolder}
        , events{&events}
        , openSession{std::move(openSession)}
        , navigate{std::move(navigate)}
    {
        documentMouseDown = Nui::bind(
            [this](Nui::val event)
            {
                if (!paneOpen.value() || rootElement.isUndefined())
                    return;
                if (!rootElement.call<bool>("contains", event["target"]))
                    closePane();
            },
            std::placeholders::_1
        );
        documentKeyDown = Nui::bind(
            [this](Nui::val event)
            {
                if (!this->events->settingsOpen.value() || inputElement.isUndefined())
                    return;
                const bool modifier = event["ctrlKey"].as<bool>() || event["metaKey"].as<bool>();
                if (modifier && event["key"].as<std::string>() == "f")
                {
                    event.call<void>("preventDefault");
                    inputElement.call<void>("focus");
                    inputElement.call<void>("select");
                }
            },
            std::placeholders::_1
        );
        settingsOpenListener = Nui::smartListen(
            this->events->settingsOpen,
            [this](bool open)
            {
                if (!open && paneOpen.value())
                {
                    drilledResult.reset();
                    setInput(query, searchText("placeholder"));
                    paneOpen = false;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            }
        );

        auto document = Nui::val::global("document");
        document.call<void>("addEventListener", "mousedown"s, documentMouseDown);
        document.call<void>("addEventListener", "keydown"s, documentKeyDown);
    }

    ~Implementation()
    {
        auto document = Nui::val::global("document");
        document.call<void>("removeEventListener", "mousedown"s, documentMouseDown);
        document.call<void>("removeEventListener", "keydown"s, documentKeyDown);
    }
    Implementation(Implementation const&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation const&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    std::size_t selectableCount() const
    {
        if (drilledResult)
            return pinnedTargets.size() + listedTargets.size();
        return results.size();
    }

    void redraw()
    {
        revision = revision.value() + 1;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    void select(std::size_t newSelection)
    {
        selection = newSelection;
        Nui::globalEventContext.executeActiveEventsImmediately();
        scrollSelectionIntoView();
    }

    void scrollSelectionIntoView()
    {
        if (rootElement.isUndefined())
            return;
        auto row = rootElement.call<Nui::val>("querySelector", ".settings-search-row.selected"s);
        if (row.isNull() || row.isUndefined())
            return;
        auto options = Nui::val::object();
        options.set("block", "nearest"s);
        row.call<void>("scrollIntoView", options);
    }

    void setInput(std::string const& value, std::string const& placeholder)
    {
        if (inputElement.isUndefined())
            return;
        inputElement.set("value", value);
        inputElement.set("placeholder", placeholder);
    }

    void onInput(std::string text)
    {
        if (drilledResult)
        {
            serverFilter = std::move(text);
            filterTargets();
            selection = 0u;
            redraw();
            return;
        }
        query = std::move(text);
        results = index->search(query, maximumResults);
        selection = 0u;
        paneOpen = !query.empty();
        redraw();
    }

    void closePane()
    {
        if (drilledResult)
            leaveTargets();
        paneOpen = false;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    void openPane()
    {
        if (query.empty())
            return;
        results = index->search(query, maximumResults);
        paneOpen = true;
        redraw();
    }

    TargetRow serverRow(std::string const& sessionId, Persistence::SessionOptions const& session) const
    {
        return TargetRow{
            .target = Target{.scope = SettingScope::Session, .sessionId = sessionId},
            .name = sessionId,
            .iconName = session.icon,
        };
    }

    void enterTargets(std::size_t resultIndex)
    {
        auto const& entry = *results[resultIndex].entry;
        drilledResult = resultIndex;
        serverFilter.clear();
        pinnedTargets.clear();
        allServerTargets.clear();

        if (auto const* inheritable = entry.renderedScope(SettingScope::Inheritable))
        {
            std::string note{};
            if (inheritable->groupKey && inheritable->groupKey->value())
                note = formatWithArguments(searchText("groupNote"), {*inheritable->groupKey->value()});
            pinnedTargets.push_back(
                TargetRow{
                    .target = Target{.scope = SettingScope::Inheritable},
                    .name = searchText("inheritableDefaults"),
                    .note = std::move(note),
                }
            );
        }

        if (auto const* session = entry.renderedScope(SettingScope::Session))
        {
            const nlohmann::json state = stateHolder->stateCache();
            const bool pathKnown = isPathKnown(state, entry.path);
            const auto open = openSession();
            auto const* sessionsJson = findPath(state, {"sessions"});

            for (auto const& [sessionId, sessionOptions] : stateHolder->stateCache().sessions)
            {
                if (session->engineType && sessionOptions.type != *session->engineType)
                    continue;

                auto row = serverRow(sessionId, sessionOptions);
                if (pathKnown && sessionsJson && sessionsJson->contains(sessionId))
                    row.status = statusFor(entry, (*sessionsJson)[sessionId]);

                if (open && *open == sessionId)
                {
                    row.note = searchText("openNote");
                    pinnedTargets.push_back(std::move(row));
                }
                else
                    allServerTargets.push_back(std::move(row));
            }
        }

        filterTargets();
        selection = 0u;
        setInput("", searchText("filterServersPlaceholder"));
        redraw();
        if (!inputElement.isUndefined())
            inputElement.call<void>("focus");
    }

    std::string statusFor(SettingsSearchIndex::Entry const& entry, nlohmann::json const& session) const
    {
        auto const* value = sessionValue(session, entry.path);
        if (!isInheritableGroup(entry.path.front()))
            return value ? displayValue(*value) : std::string{};

        if (value)
            return formatWithArguments(searchText("overrides"), {displayValue(*value)});
        auto const* group = inlineGroup(session, entry.path.front());
        auto const* reference = group ? findPath(*group, {"$ref"}) : nullptr;
        if (reference && reference->is_string())
            return formatWithArguments(searchText("inheritsFrom"), {reference->get<std::string>()});
        return searchText("inherits");
    }

    void filterTargets()
    {
        listedTargets.clear();
        const auto filter = Utility::FuzzySearch::normalize(serverFilter);
        for (auto const& row : allServerTargets)
        {
            if (filter.empty() || Utility::FuzzySearch::normalize(row.name).find(filter) != std::u32string::npos)
                listedTargets.push_back(row);
        }
    }

    void leaveTargets()
    {
        drilledResult.reset();
        serverFilter.clear();
        selection = 0u;
        setInput(query, searchText("placeholder"));
        redraw();
    }

    TargetRow const* selectedTarget() const
    {
        const auto position = selection.value();
        if (position < pinnedTargets.size())
            return &pinnedTargets[position];
        if (position - pinnedTargets.size() < listedTargets.size())
            return &listedTargets[position - pinnedTargets.size()];
        return nullptr;
    }

    void activate()
    {
        if (drilledResult)
        {
            auto const* row = selectedTarget();
            if (!row)
                return;
            auto const* entry = results[*drilledResult].entry;
            const auto target = row->target;
            clearAndClose();
            navigate(*entry, target);
            return;
        }

        if (selection.value() >= results.size())
            return;
        auto const& entry = *results[selection.value()].entry;
        if (needsTargetChoice(entry))
        {
            enterTargets(selection.value());
            return;
        }
        clearAndClose();
        navigate(entry, Target{.scope = SettingScope::General});
    }

    /**
     * @brief Empties the search box and closes the pane once a setting was picked.
     */
    void clearAndClose()
    {
        query.clear();
        drilledResult.reset();
        serverFilter.clear();
        selection = 0u;
        setInput("", searchText("placeholder"));
        paneOpen = false;
        Nui::globalEventContext.executeActiveEventsImmediately();
        results.clear();
    }

    void onKeyDown(Nui::val event)
    {
        const auto key = event["key"].as<std::string>();
        const auto count = selectableCount();
        const auto caret = inputElement.isUndefined() ? 0 : inputElement["selectionStart"].as<int>();
        const auto valueLength =
            inputElement.isUndefined() ? 0 : static_cast<int>(inputElement["value"].as<std::string>().size());

        const auto consume = [&event]()
        {
            event.call<void>("preventDefault");
            event.call<void>("stopPropagation");
        };

        if (key == "ArrowDown" && paneOpen.value())
        {
            consume();
            if (count > 0u)
                select(std::min(selection.value() + 1u, count - 1u));
        }
        else if (key == "ArrowUp" && paneOpen.value())
        {
            consume();
            if (selection.value() > 0u)
                select(selection.value() - 1u);
        }
        else if (key == "Enter")
        {
            consume();
            if (!paneOpen.value())
                openPane();
            else
                activate();
        }
        else if (
            key == "ArrowRight" && paneOpen.value() && !drilledResult && caret == valueLength &&
            selection.value() < results.size() && needsTargetChoice(*results[selection.value()].entry)
        )
        {
            consume();
            enterTargets(selection.value());
        }
        else if (drilledResult && ((key == "ArrowLeft" && caret == 0) || (key == "Backspace" && valueLength == 0)))
        {
            consume();
            leaveTargets();
        }
        else if (key == "Escape")
        {
            consume();
            if (drilledResult)
                leaveTargets();
            else
                closePane();
        }
    }

    std::string breadcrumbOf(SettingsSearchIndex::Entry const& entry) const
    {
        const auto scope = primaryScope(entry);
        if (!scope)
            return {};
        std::vector<std::string> parts;
        switch (*scope)
        {
            case SettingScope::General:
                parts.push_back(searchText("scopeGeneral"));
                break;
            case SettingScope::Inheritable:
                parts.push_back(searchText("scopeInheritable"));
                break;
            case SettingScope::Session:
                parts.push_back(searchText("scopeSession"));
                break;
        }
        for (auto const& crumb : entry.renderedScope(*scope)->breadcrumb)
            parts.push_back(translated(crumb));
        return fmt::format("{}", fmt::join(parts, " › "));
    }

    std::string badgeOf(SettingsSearchIndex::Entry const& entry) const
    {
        if (entry.renderedScope(SettingScope::Inheritable))
            return searchText("badgeDefaultsAndServers");
        if (entry.renderedScope(SettingScope::Session))
            return searchText("badgeServerOnly");
        return searchText("badgeGeneral");
    }

    Nui::Attribute rowClass(std::size_t position)
    {
        using namespace Nui::Attributes;
        return class_ = Nui::observe(selection).generate(
                   [position](std::size_t selected)
                   {
                       return position == selected ? "settings-search-row selected"s : "settings-search-row"s;
                   }
               );
    }

    Nui::ElementRenderer renderResult(std::size_t position)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        auto const& result = results[position];
        auto const& entry = *result.entry;
        const auto label = translated(entry.labelKey);
        const auto detail = result.match.field == Utility::FuzzySearch::MatchedField::Tag
            ? formatWithArguments(searchText("viaTag"), {result.matchedTag})
            : translated(entry.helpTextKey);

        // clang-format off
        return div{
            rowClass(position),
            onClick = [this, position]() {
                selection = position;
                activate();
            },
        }(
            div{class_ = "settings-search-row-head"}(
                CommandPanels::highlightedText(label, CommandPanels::lowercased(query), "settings-search-label"),
                span{class_ = "settings-search-badge"}(badgeOf(entry))
            ),
            div{class_ = "settings-search-breadcrumb"}(breadcrumbOf(entry)),
            div{class_ = "settings-search-detail"}(detail),
            needsTargetChoice(entry)
                ? Nui::ElementRenderer{span{class_ = "settings-search-chevron"}(Ui5Icons::slim_arrow_right())}
                : Nui::nil()
        );
        // clang-format on
    }

    Nui::ElementRenderer renderTarget(TargetRow const& row, std::size_t position)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        Nui::ElementRenderer icon = row.target.scope == SettingScope::Inheritable ? GeneratedSvgs::settings()
            : row.iconName.empty()                                                ? GeneratedSvgs::itsystem()
                                                                                  : iconFromName(row.iconName);

        // clang-format off
        return div{
            rowClass(position),
            onClick = [this, position]() {
                selection = position;
                activate();
            },
        }(
            span{class_ = "settings-search-target-icon"}(std::move(icon)),
            span{class_ = "settings-search-target-name"}(row.name),
            row.note.empty() ? Nui::nil()
                             : Nui::ElementRenderer{span{class_ = "settings-search-target-note"}(row.note)},
            span{class_ = "settings-search-target-status"}(row.status)
        );
        // clang-format on
    }

    Nui::ElementRenderer renderResults()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if (results.empty())
            return div{class_ = "settings-search-empty"}(searchText("noResults"));

        std::vector<Nui::ElementRenderer> rows;
        for (std::size_t position = 0u; position < results.size(); ++position)
            rows.push_back(renderResult(position));
        return div{class_ = "settings-search-list"}(Nui::range(std::move(rows)), passThrough);
    }

    Nui::ElementRenderer renderTargets()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        auto const& entry = *results[*drilledResult].entry;

        std::vector<Nui::ElementRenderer> pinnedRows;
        for (std::size_t position = 0u; position < pinnedTargets.size(); ++position)
            pinnedRows.push_back(renderTarget(pinnedTargets[position], position));

        std::vector<Nui::ElementRenderer> listedRows;
        for (std::size_t position = 0u; position < listedTargets.size(); ++position)
            listedRows.push_back(renderTarget(listedTargets[position], pinnedTargets.size() + position));

        const bool nothingToPick = pinnedTargets.empty() && allServerTargets.empty();
        const bool filteredAway = !allServerTargets.empty() && listedTargets.empty();

        // clang-format off
        return div{class_ = "settings-search-targets"}(
            div{
                class_ = "settings-search-back",
                onClick = [this]() {
                    leaveTargets();
                    if (!inputElement.isUndefined())
                        inputElement.call<void>("focus");
                },
            }(
                Ui5Icons::nav_back(),
                span{}(translated(entry.labelKey))
            ),
            div{class_ = "settings-search-pinned"}(Nui::range(std::move(pinnedRows)), passThrough),
            nothingToPick ? Nui::ElementRenderer{div{class_ = "settings-search-empty"}(searchText("noServers"))}
            : filteredAway
                ? Nui::ElementRenderer{div{class_ = "settings-search-empty"}(searchText("noServersMatch"))}
                : Nui::ElementRenderer{div{class_ = "settings-search-list settings-search-server-list"}(
                      Nui::range(std::move(listedRows)), passThrough
                  )}
        );
        // clang-format on
    }

    Nui::ElementRenderer renderPane()
    {
        if (!paneOpen.value())
            return Nui::nil();
        if (drilledResult)
            return renderTargets();
        return renderResults();
    }
};

SettingsSearch::SettingsSearch(
    SettingsSearchIndex& index,
    Persistence::StateHolder& stateHolder,
    FrontendEvents& events,
    std::function<std::optional<std::string>()> openSession,
    NavigateFunction navigate
)
    : impl_{std::make_unique<Implementation>(index, stateHolder, events, std::move(openSession), std::move(navigate))}
{}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SettingsSearch);

Nui::ElementRenderer SettingsSearch::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        class_ = "settings-search",
        reference.onMaterialize([this](Nui::val element) {
            impl_->rootElement = std::move(element);
        }),
    }(
        div{class_ = "settings-search-box"}(
            Ui5Icons::search(),
            input{
                type = "text",
                class_ = "settings-search-input",
                placeHolder = searchText("placeholder"),
                Nui::Attributes::title = searchText("shortcutHint"),
                disabled = Nui::observe(impl_->events->settingsInitialLoadComplete).generate([](bool complete) {
                    return !complete;
                }),
                reference.onMaterialize([this](Nui::val element) {
                    impl_->inputElement = std::move(element);
                }),
                onInput = [this](Nui::val event) {
                    if (event["isComposing"].as<bool>())
                        return;
                    impl_->onInput(event["target"]["value"].as<std::string>());
                },
                "compositionend"_event = [this](Nui::val event) {
                    impl_->onInput(event["target"]["value"].as<std::string>());
                },
                onFocus = [this]() {
                    if (!impl_->paneOpen.value())
                        impl_->openPane();
                },
                onKeyDown = [this](Nui::val event) {
                    impl_->onKeyDown(std::move(event));
                },
            }()
        ),
        div{
            class_ = Nui::observe(impl_->paneOpen).generate([](bool open) {
                return open ? "settings-search-pane open"s : "settings-search-pane"s;
            }),
        }(
            Nui::observe(impl_->paneOpen, impl_->revision).generate([this]() -> Nui::ElementRenderer {
                return impl_->renderPane();
            })
        )
    );
    // clang-format on
}
