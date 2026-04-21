#include <frontend/session_components/session_layout_initializer.hpp>

#include <frontend/session_components/terminal_panel.hpp>
#include <frontend/session_components/file_explorer_panel.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/session_options.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <frontend/icon_from_name.hpp>
#include <persistence/state_holder.hpp>
#include <constants/layouts.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/popup_menu.hpp>

#include <fmt/format.h>

#include <nui-file-explorer/file_grid.hpp>
#include <nui-file-explorer/side.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/dom/element.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/val.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

struct SessionLayoutInitializer::Implementation
{
    Persistence::StateHolder* stateHolder;
    ConfirmDialog* confirmDialog;
    std::string sessionLayoutId;
    std::optional<std::string> layoutName;
    Persistence::SessionOptions const* engineOptions;
    Nui::Observed<std::unique_ptr<FrontendSessionManager>>* frontendSessionManager;
    Nui::Observed<bool>* isInLostConnectionState;
    TerminalPanel* terminalPanel;
    FileExplorerPanel* fileExplorerPanel;
    OperationQueue* operationQueue;
    FileTrackingPanel* fileTrackingPanel;
    SessionOptions* sessionOptions;
    std::vector<LocalShellAdoption>* pendingLocalShellAdoptions;
    std::function<std::optional<nlohmann::json>()> takeResumeLayout;
    std::function<void()> onLayoutCreationFailed;

    ScriptNuiComponents::PopupMenu tabAddMenu{};

    // "Currently mounted" DOM handles for the single-instance panels.  The
    // smartListens below flip the matching tab-add-menu entries disabled.
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> operationQueueElement{};
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> fileTrackingElement{};
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> sessionOptionsElement{};

    Nui::ListenRemover<decltype(operationQueueElement)> operationQueueListener{};
    Nui::ListenRemover<decltype(fileTrackingElement)> fileTrackingListener{};
    Nui::ListenRemover<decltype(sessionOptionsElement)> sessionOptionsListener{};
    Nui::ListenRemover<Nui::Observed<std::shared_ptr<Nui::Dom::Element>>> fileExplorerListener{};

    std::weak_ptr<Nui::Dom::BasicElement> layoutHost;
    bool waitingForLayoutHost{false};

    explicit Implementation(Params&& params)
        : stateHolder{params.stateHolder}
        , confirmDialog{params.confirmDialog}
        , sessionLayoutId{std::move(params.sessionLayoutId)}
        , layoutName{std::move(params.layoutName)}
        , engineOptions{params.engineOptions}
        , frontendSessionManager{params.frontendSessionManager}
        , isInLostConnectionState{params.isInLostConnectionState}
        , terminalPanel{params.terminalPanel}
        , fileExplorerPanel{params.fileExplorerPanel}
        , operationQueue{params.operationQueue}
        , fileTrackingPanel{params.fileTrackingPanel}
        , sessionOptions{params.sessionOptions}
        , pendingLocalShellAdoptions{params.pendingLocalShellAdoptions}
        , takeResumeLayout{std::move(params.takeResumeLayout)}
        , onLayoutCreationFailed{std::move(params.onLayoutCreationFailed)}
    {}
};

namespace
{
    /**
     * @brief Wraps a panel's renderer with the SFTP-bound view blocker that
     *        dims it during a reconnect cycle.
     */
    Nui::ElementRenderer withBlocker(
        Nui::ElementRenderer body,
        Nui::Observed<bool>& isInLostConnectionState
    )
    {
        using Nui::Elements::div;
        using Nui::Attributes::class_;
        using Nui::Attributes::style;

        // clang-format off
        return div{
            style = "width: 100%; height: 100%; position: relative; display: block",
        }(
            std::move(body),
            div{
                style = observe(isInLostConnectionState).generate([&isInLostConnectionState](){
                    return fmt::format("display: {};", isInLostConnectionState.value() ? "block" : "none");
                }),
                class_ = "session-panel-blocker",
            }()
        );
        // clang-format on
    }
}

SessionLayoutInitializer::SessionLayoutInitializer(Params params)
    : impl_{std::make_unique<Implementation>(std::move(params))}
{
    // Wire the four panel-mount listeners so menu entries flip disabled
    // when their panel appears.  openAddContextMenu repopulates items.
    auto modifyEntry = [this](std::string const& label, bool disabled) {
        impl_->tabAddMenu.modifyItemByLabel(
            label,
            [disabled](ScriptNuiComponents::PopupMenu::MenuItem* mi) {
                if (mi)
                    mi->disabled = disabled;
            }
        );
    };

    impl_->sessionOptionsListener = Nui::smartListen(
        impl_->sessionOptionsElement,
        [modifyEntry](std::shared_ptr<Nui::Dom::Element> const& elem)
        {
            modifyEntry(language->get("sessionFrontend", "sessionOptions"), elem != nullptr);
        }
    );
    impl_->fileExplorerListener = Nui::smartListen(
        impl_->fileExplorerPanel->elementObservable(),
        [modifyEntry](std::shared_ptr<Nui::Dom::Element> const& elem)
        {
            Nui::WebApi::Console::log("fileExplorerElement changed.");
            modifyEntry(language->get("sessionFrontend", "fileExplorer"), elem != nullptr);
        }
    );
    impl_->operationQueueListener = Nui::smartListen(
        impl_->operationQueueElement,
        [modifyEntry](std::shared_ptr<Nui::Dom::Element> const& elem)
        {
            Nui::WebApi::Console::log("operationQueueElement changed.");
            modifyEntry(language->get("sessionFrontend", "operationQueue"), elem != nullptr);
        }
    );
    impl_->fileTrackingListener = Nui::smartListen(
        impl_->fileTrackingElement,
        [modifyEntry](std::shared_ptr<Nui::Dom::Element> const& elem)
        {
            modifyEntry(language->get("sessionFrontend", "fileTracking"), elem != nullptr);
        }
    );
}
SessionLayoutInitializer::~SessionLayoutInitializer() = default;
SessionLayoutInitializer::SessionLayoutInitializer(SessionLayoutInitializer&&) = default;
SessionLayoutInitializer& SessionLayoutInitializer::operator=(SessionLayoutInitializer&&) = default;

void SessionLayoutInitializer::rebuildTabAddMenuInto(Implementation& impl)
{
        using namespace ScriptNuiComponents;
        using namespace std::string_literals;

        const bool isSsh = std::holds_alternative<Persistence::SshSessionOptions>(impl.engineOptions->engine);

        std::vector<PopupMenu::Entry> items;
        items.reserve(8);

        items.push_back(PopupMenu::sectionHeader(language->get("sessionFrontend", "newTab")));

        items.push_back(PopupMenu::item(
            language->get("sessionFrontend", "terminal"),
            std::string{},
            [&impl]()
            {
                Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "terminal"s);
                impl.tabAddMenu.close();
            }
        ));

        items.push_back(PopupMenu::item(
            language->get("sessionFrontend", "fileExplorer"),
            std::string{},
            [&impl]()
            {
                impl.tabAddMenu.close();
                if (impl.fileExplorerPanel->elementObservable().value())
                    return;
                Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "file-explorer"s);
            },
            impl.fileExplorerPanel->elementObservable().value() != nullptr
        ));

        if (isSsh)
        {
            items.push_back(PopupMenu::item(
                language->get("sessionFrontend", "operationQueue"),
                std::string{},
                [&impl]()
                {
                    impl.tabAddMenu.close();
                    if (impl.operationQueueElement.value())
                        return;
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "operation-queue"s);
                },
                impl.operationQueueElement.value() != nullptr
            ));
            items.push_back(PopupMenu::item(
                language->get("sessionFrontend", "fileTracking"),
                std::string{},
                [&impl]()
                {
                    impl.tabAddMenu.close();
                    if (impl.fileTrackingElement.value())
                        return;
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "file-tracking"s);
                },
                impl.fileTrackingElement.value() != nullptr
            ));

            // Dynamic Local Shell section: one entry per saved shell-type SessionOptions.
            std::vector<std::pair<std::string, std::string>> shells; // (name, icon)
            for (auto const& [name, sess] : impl.stateHolder->stateCache().sessions)
            {
                if (sess.type == Persistence::TerminalEngineType::shell &&
                    std::holds_alternative<Persistence::ExecutingSessionOptions>(sess.engine))
                {
                    shells.emplace_back(name, sess.icon);
                }
            }

            items.push_back(PopupMenu::separator());
            items.push_back(PopupMenu::sectionHeader(language->get("sessionFrontend", "localShell")));

            if (shells.empty())
            {
                items.push_back(PopupMenu::item(
                    language->get("sessionFrontend", "noLocalShellsConfigured"),
                    std::string{},
                    []() {},
                    /*disabled=*/true,
                    std::string{},
                    language->get("sessionFrontend", "noLocalShellsTooltip")
                ));
            }
            else
            {
                for (auto const& [name, icon] : shells)
                {
                    items.push_back(PopupMenu::item(
                        name,
                        icon.empty() ? Nui::nil() : iconFromName(icon),
                        [&impl, name]()
                        {
                            impl.tabAddMenu.close();
                            Nui::val::global("contentPanelManager")
                                .call<void>("fullfillLastAddRequest", "local-shell:"s + name);
                        }
                    ));
                }
            }
        }

    impl.tabAddMenu.setItems(std::move(items));
}

void SessionLayoutInitializer::attachLayoutHost(std::weak_ptr<Nui::Dom::BasicElement> host)
{
    impl_->layoutHost = host;
    if (impl_->waitingForLayoutHost)
    {
        impl_->waitingForLayoutHost = false;
        Nui::globalEventContext.delayToAfterProcessing([this]() {
            Log::info("Layout host attached to DOM, initializing layout");
            initialize();
        });
    }
}

void SessionLayoutInitializer::initialize()
{
    Log::info("Trying to initialize session layout...");

    Nui::val element;
    if (auto host = impl_->layoutHost.lock(); host)
    {
        Log::info("Layout host found, initializing layout");
        element = host->val();
    }
    else
    {
        Log::info("Waiting for layout host");
        impl_->waitingForLayoutHost = true;
        return;
    }

    std::optional<nlohmann::json> layout = std::nullopt;

    // Reconnect path: prefer the snapshot's layout to round-trip the exact
    // set of panels that were open at the moment of disconnection.
    if (impl_->takeResumeLayout)
        layout = impl_->takeResumeLayout();

    if (!layout && impl_->layoutName != Constants::defaultLayoutName)
    {
        if (impl_->engineOptions->layouts && impl_->layoutName)
        {
            if (auto iter = impl_->engineOptions->layouts->find(*impl_->layoutName);
                iter != impl_->engineOptions->layouts->end())
            {
                layout = iter->second;
            }
            else
            {
                Log::warn("Layout name not found: {}", *impl_->layoutName);
            }
        }
    }

    Log::info(
        "Initializing layout with name '{}': {}",
        impl_->layoutName.value_or("(none)"),
        layout ? layout->dump() : "(none)"
    );

    // Restore file-grid flavor persisted in the __extra blob.
    if (layout && layout->contains("__extra"))
    {
        auto const& layoutExtra = (*layout)["__extra"];
        if (layoutExtra.contains("fileGrid"))
        {
            auto fileGridExtra = layoutExtra["fileGrid"];
            if (fileGridExtra.contains("leftSide") && fileGridExtra["leftSide"].contains("flavor"))
            {
                impl_->fileExplorerPanel->localFileGridSide().flavor(
                    NuiFileExplorer::fileGridFlavorFromString(fileGridExtra["leftSide"]["flavor"].get<std::string>())
                );
            }
            if (fileGridExtra.contains("rightSide") && fileGridExtra["rightSide"].contains("flavor") &&
                impl_->fileExplorerPanel->remoteFileGridSide())
            {
                impl_->fileExplorerPanel->remoteFileGridSide()->flavor(
                    NuiFileExplorer::fileGridFlavorFromString(fileGridExtra["rightSide"]["flavor"].get<std::string>())
                );
            }
        }
    }

    auto addPanelArgument = Nui::val::object();
    addPanelArgument.set("host", element);
    addPanelArgument.set("id", impl_->sessionLayoutId);
    addPanelArgument.set("engineType", impl_->frontendSessionManager->value()->engine().engineName());
    addPanelArgument.set("layoutString", layout ? Nui::val(layout->dump()) : Nui::val::undefined());
    addPanelArgument.set(
        "terminalFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                Nui::WebApi::Console::log("Channel factory content panel manager");
                auto elem = Nui::Dom::makeStandaloneElement(impl_->terminalPanel->makeChannelElement());
                impl_->terminalPanel->channelElements().push_back(elem);
                return elem->val();
            }
        )
    );
    addPanelArgument.set(
        "terminalDelete",
        Nui::bind(
            [this](Nui::val channelIdVal) -> Nui::val
            {
                Nui::WebApi::Console::log(channelIdVal);

                if (channelIdVal.isUndefined())
                {
                    Log::critical("Channel id is undefined");
                    return Nui::val::undefined();
                }

                if (channelIdVal.isString())
                {
                    Ids::ChannelId channelId = Ids::makeChannelId(channelIdVal.as<std::string>());
                    if (!channelId.isValid())
                    {
                        Log::critical("Channel id is not valid");
                        return Nui::val::undefined();
                    }

                    impl_->terminalPanel->onChannelClosedByUser(channelId);
                }
                else
                {
                    Log::critical("Channel id is not a string");
                }
                return Nui::val::undefined();
            },
            std::placeholders::_1
        )
    );
    addPanelArgument.set(
        "fileExplorerFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                auto& fileExplorerElement = impl_->fileExplorerPanel->elementObservable();
                if (fileExplorerElement.value())
                {
                    Log::warn("There is already a file explorer, cannot open another one");
                    return Nui::val::undefined();
                }
                fileExplorerElement = Nui::Dom::makeStandaloneElement(impl_->fileExplorerPanel->makeFileExplorerElement());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return fileExplorerElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "fileExplorerDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                auto& fileExplorerElement = impl_->fileExplorerPanel->elementObservable();
                if (!fileExplorerElement.value())
                {
                    Log::warn("There is no file explorer to remove");
                    return Nui::val::undefined();
                }
                Nui::WebApi::Console::log("Removing file explorer element");
                fileExplorerElement.value().reset();
                fileExplorerElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "operationQueueFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (impl_->operationQueueElement.value())
                {
                    Log::warn("There is already an operation queue, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->operationQueueElement = Nui::Dom::makeStandaloneElement(
                    withBlocker((*impl_->operationQueue)(), *impl_->isInLostConnectionState)
                );
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->operationQueueElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "operationQueueDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (!impl_->operationQueueElement.value())
                {
                    Log::warn("There is no operation queue to remove");
                    return Nui::val::undefined();
                }
                impl_->operationQueueElement.value().reset();
                impl_->operationQueueElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "sessionOptionsFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (impl_->sessionOptionsElement.value())
                {
                    Log::warn("There are already session options, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->sessionOptionsElement = Nui::Dom::makeStandaloneElement((*impl_->sessionOptions)());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->sessionOptionsElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "sessionOptionsDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (!impl_->sessionOptionsElement.value())
                {
                    Log::warn("There are no session options to remove");
                    return Nui::val::undefined();
                }
                impl_->sessionOptionsElement.value().reset();
                impl_->sessionOptionsElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "fileTrackingFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (impl_->fileTrackingElement.value())
                {
                    Log::warn("There is already a file tracking panel, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->fileTrackingElement = Nui::Dom::makeStandaloneElement(
                    withBlocker((*impl_->fileTrackingPanel)(), *impl_->isInLostConnectionState)
                );
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->fileTrackingElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "fileTrackingDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (!impl_->fileTrackingElement.value())
                {
                    Log::warn("There is no file tracking panel to remove");
                    return Nui::val::undefined();
                }
                impl_->fileTrackingElement.value().reset();
                impl_->fileTrackingElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "localShellFactory",
        Nui::bind(
            [this](Nui::val shellNameVal) -> Nui::val
            {
                if (!shellNameVal.isString())
                {
                    Log::error("localShellFactory called without shell name string");
                    return Nui::val::undefined();
                }
                const std::string shellName = shellNameVal.as<std::string>();

                // Reconnect: adopt a pending entry for this shell rather than
                // spawning fresh.  First match wins (materialization-order = capture-order).
                if (impl_->pendingLocalShellAdoptions && !impl_->pendingLocalShellAdoptions->empty())
                {
                    auto match = std::ranges::find_if(
                        *impl_->pendingLocalShellAdoptions,
                        [&shellName](LocalShellAdoption const& adoption) {
                            return adoption.shellConfigName == shellName;
                        }
                    );
                    if (match != impl_->pendingLocalShellAdoptions->end())
                    {
                        Log::info("localShellFactory: adopting local shell '{}'", shellName);
                        LocalShellAdoption adoption = std::move(*match);
                        impl_->pendingLocalShellAdoptions->erase(match);
                        auto elem = Nui::Dom::makeStandaloneElement(
                            impl_->terminalPanel->makeAdoptedLocalShellChannelElement(std::move(adoption))
                        );
                        impl_->terminalPanel->channelElements().push_back(elem);
                        return elem->val();
                    }
                }

                // Drop the widget if the shell config was deleted since save.
                auto const& sessions = impl_->stateHolder->stateCache().sessions;
                auto iter = sessions.find(shellName);
                if (iter == sessions.end() ||
                    !std::holds_alternative<Persistence::ExecutingSessionOptions>(iter->second.engine))
                {
                    Log::warn("localShellFactory: shell '{}' no longer in settings dropping widget", shellName);
                    return Nui::val::undefined();
                }

                Log::info("localShellFactory: spawning local shell '{}'", shellName);
                auto elem = Nui::Dom::makeStandaloneElement(
                    impl_->terminalPanel->makeLocalShellChannelElement(shellName)
                );
                impl_->terminalPanel->channelElements().push_back(elem);
                return elem->val();
            },
            std::placeholders::_1
        )
    );
    addPanelArgument.set(
        "localShellDelete",
        Nui::bind(
            [this](Nui::val channelIdVal) -> Nui::val
            {
                if (channelIdVal.isUndefined())
                {
                    Log::warn("localShellDelete: channel id undefined (never spawned)");
                    return Nui::val::undefined();
                }
                if (!channelIdVal.isString())
                {
                    Log::error("localShellDelete: channel id is not a string");
                    return Nui::val::undefined();
                }
                Ids::ChannelId channelId = Ids::makeChannelId(channelIdVal.as<std::string>());
                if (!channelId.isValid())
                {
                    Log::error("localShellDelete: channel id is invalid");
                    return Nui::val::undefined();
                }
                impl_->terminalPanel->onChannelClosedByUser(channelId);
                return Nui::val::undefined();
            },
            std::placeholders::_1
        )
    );
    addPanelArgument.set(
        "openAddContextMenu",
        Nui::bind(
            [this](Nui::val val)
            {
                Log::info("openAddContextMenu called");
                if (!val.isString())
                {
                    Log::error("openAddContextMenu needs to be called with a string argument");
                }
                // Refresh entries so the Local Shell list reflects current saved
                // shell sessions and single-instance panel state is accurate.
                rebuildTabAddMenuInto(*impl_);
                Nui::globalEventContext.executeActiveEventsImmediately();
                impl_->tabAddMenu.openNextTo(val.as<std::string>());
            },
            std::placeholders::_1
        )
    );

    Log::info("Adding panel to content panel manager with layout id '{}'", impl_->sessionLayoutId);
    const auto addPanelResult = Nui::val::global("contentPanelManager").call<bool>("addPanel", addPanelArgument);
    if (!addPanelResult)
    {
        Log::error("Failed to add panel to content panel manager");
        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "layoutCreationFailedHeader"),
            .text = language->get("sessionFrontend", "layoutCreationFailedText"),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "layoutCreationFailed",
        });
        if (impl_->onLayoutCreationFailed)
            impl_->onLayoutCreationFailed();
    }
    Log::info("Panel added to content panel manager successfully");
}

std::optional<nlohmann::json> SessionLayoutInitializer::getLayout() const
{
    // session layout id is not the name in the setting, but the id for the lumino datastructure in the
    // contentPanelManager where this session lives in.
    auto layout = Nui::val::global("contentPanelManager").call<Nui::val>("getPanelLayout", impl_->sessionLayoutId);
    if (layout.isUndefined())
        return std::nullopt;
    auto layoutObject = nlohmann::json::parse(Nui::JSON::stringify(layout));
    layoutObject["__extra"] = {{
        "fileGrid",
        {
            {
                "leftSide",
                {
                    {"flavor", fileGridFlavorToString(impl_->fileExplorerPanel->localFileGridSide().flavor())},
                },
            },
        },
    }};
    if (impl_->fileExplorerPanel->remoteFileGridSide())
    {
        layoutObject["__extra"]["fileGrid"]["rightSide"] = {
            {"flavor", fileGridFlavorToString(impl_->fileExplorerPanel->remoteFileGridSide()->flavor())},
        };
    }
    return layoutObject;
}

void SessionLayoutInitializer::openLocalShellChannel(std::string const& shellName)
{
    using namespace std::string_literals;

    // The layout id carries the shell name so saved layouts round-trip and
    // content_panel_manager can route factory calls to the right shell config.
    Nui::val::global("contentPanelManager")
        .call<void>("fullfillLastAddRequest", "local-shell:"s + shellName);
}

Nui::ElementRenderer SessionLayoutInitializer::tabAddMenuRenderer()
{
    return impl_->tabAddMenu();
}
