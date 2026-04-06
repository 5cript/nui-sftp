#include <persistence/state/session_options.hpp>
#include <frontend/session.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <frontend/terminal/executing_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/classes.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/session_components/session_options.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/session_components/file_tracking.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <nui-file-explorer/file_grid.hpp>
#include <persistence/state_holder.hpp>
#include <constants/layouts.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/popup_menu.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/utility/delocalized.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/filesystem/file_dialog.hpp>

#include <algorithm>
#include <vector>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

struct Session::Implementation
{
    // Prop Drill:
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;

    // Session Ui Tab related:
    std::string initialName;
    std::shared_ptr<Nui::Observed<std::string>> tabTitle;
    std::string sessionLayoutId;
    std::function<void(Session const*)> closeSelf;
    Nui::Observed<bool> isVisible;
    int tabId;
    Persistence::UiOptions uiOptions;

    // Session Options Tab
    Persistence::SessionOptions engineOptions;
    Nui::Observed<Persistence::TerminalOptions> options;

    // Add Tab Context Menu
    ScriptNuiComponents::PopupMenu tabAddMenu{};

    // Dialogs:
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;

    // File Explorer Things:
    NuiFileExplorer::FileGrid fileGrid;
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> fileExplorerElement;

    // Operation Queue for File Explorer
    OperationQueue operationQueue;
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> operationQueueElement;

    // Layout Engine Related
    std::weak_ptr<Nui::Dom::BasicElement> layoutHost;
    std::optional<std::string> layoutName;
    bool waitingForLayoutHost{false};

    // Channels & FrontendSessionManager Connection
    Nui::Observed<std::unique_ptr<FrontendSessionManager>> frontendSessionManager;
    std::vector<std::shared_ptr<Nui::Dom::Element>> channelElements;

    // Session Options
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> sessionOptionsElement{};
    SessionOptions sessionOptions;

    // File Tracking
    FileTrackingPanel fileTrackingPanel;
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> fileTrackingElement{};

    // Shutdown & Connection Status:
    Nui::Observed<bool> inertEverything{false};
    Nui::Observed<bool> isInLostConnectionState{false};
    std::function<void()> onShutdownComplete{};
    std::vector<std::string> savedTerminalContents{};

    std::function<std::string(Session const* ptr, std::string const&)> disambiguateTitle;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        Persistence::SessionOptions engineOptions,
        Persistence::UiOptions uiOptions,
        std::string initialName,
        std::optional<std::string> layoutName,
        InputDialog* inputDialog,
        ConfirmDialog* confirmDialog,
        FilePropertyDialog* filePropertyDialog,
        std::function<void(Session const*)> closeSelf,
        std::function<std::string(Session const* ptr, std::string const&)> disambiguateTitle,
        bool visible,
        int tabId)
        : stateHolder{stateHolder}
        , events{events}
        , initialName{std::move(initialName)}
        , tabTitle{std::make_shared<Nui::Observed<std::string>>(this->initialName)}
        , sessionLayoutId{Nui::val::global("generateId")().as<std::string>()}
        , closeSelf{std::move(closeSelf)}
        , isVisible{visible}
        , tabId{tabId}
        , uiOptions{uiOptions}
        , engineOptions{std::move(engineOptions)}
        , options{this->engineOptions.terminalOptions.value()}
        , inputDialog{inputDialog}
        , confirmDialog{confirmDialog}
        , fileGrid{{
              .pathBarOnTop = uiOptions.fileGridPathBarOnTop,
              .showHiddenFiles = uiOptions.showHiddenFilesLocally,
              .onShowHiddenFilesChanged = [this](bool value) {
                  this->stateHolder->loadModifySave([value](Persistence::State& state) {
                      state.uiOptions.showHiddenFilesLocally = value;
                  });
              },
         }, {
                .pathBarOnTop = uiOptions.fileGridPathBarOnTop,
                .showHiddenFiles = uiOptions.showHiddenFilesRemotely,
                .onShowHiddenFilesChanged = [this](bool value) {
                    this->stateHolder->loadModifySave([value](Persistence::State& state) {
                        state.uiOptions.showHiddenFilesRemotely = value;
                    });
                },
         },
            std::make_unique<LocalSideModel>(this->uiOptions, confirmDialog, inputDialog, filePropertyDialog),
            std::make_unique<RemoteSideModel>(this->uiOptions, confirmDialog, inputDialog, filePropertyDialog),
        }
        , fileExplorerElement{}
        , operationQueue{this->stateHolder, this->events, this->initialName, this->confirmDialog, static_cast<LocalSideModel*>(&fileGrid.leftModel()), static_cast<RemoteSideModel*>(&fileGrid.rightModel())}
        , operationQueueElement{}
        , layoutHost{}
        , layoutName{std::move(layoutName)}
        , frontendSessionManager{}
        , channelElements{}
        , sessionOptionsElement{}
        , sessionOptions{stateHolder, events, this->initialName, this->sessionLayoutId, confirmDialog}
        , fileTrackingPanel{stateHolder, events, confirmDialog}
        , fileTrackingElement{}
        , disambiguateTitle{std::move(disambiguateTitle)}
    {
        fileGrid.leftModel().dropMetadata(sessionLayoutId);
        fileGrid.rightModel().dropMetadata(sessionLayoutId);

        using namespace ScriptNuiComponents;
        using namespace std::string_literals;

        tabAddMenu.setItems({
            PopupMenu::sectionHeader("New Tab"),
            PopupMenu::item(
                language->get("sessionFrontend", "terminal"),
                {},
                [this]()
                {
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "terminal"s);
                    tabAddMenu.close();
                }
            ),
            PopupMenu::item(
                language->get("sessionFrontend", "fileExplorer"),
                {},
                [this]()
                {
                    tabAddMenu.close();
                    // There can only be one!
                    if (fileExplorerElement.value())
                        return;
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "file-explorer"s);
                }
            ),
            PopupMenu::item(
                language->get("sessionFrontend", "operationQueue"),
                {},
                [this]()
                {
                    tabAddMenu.close();
                    // There can only be one!
                    if (operationQueueElement.value())
                        return;
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "operation-queue"s);
                }
            ),
            PopupMenu::item(
                language->get("sessionFrontend", "sessionOptions"),
                {},
                [this]()
                {
                    tabAddMenu.close();
                    // There can only be one!
                    if (sessionOptionsElement.value())
                        return;
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "session-options"s);
                }
            ),
            PopupMenu::item(
                language->get("sessionFrontend", "fileTracking"),
                {},
                [this]()
                {
                    tabAddMenu.close();
                    // There can only be one!
                    if (fileTrackingElement.value())
                        return;
                    Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "file-tracking"s);
                }
            ),
        });

        Nui::listen(
            sessionOptionsElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                sessionOptionsElement.eventContext().delayToAfterProcessing(
                    [this, elem]()
                    {
                        tabAddMenu.modifyItemByLabel(
                            language->get("sessionFrontend", "sessionOptions"),
                            [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                            {
                                if (mi)
                                    mi->disabled = dis;
                            }
                        );
                    }
                );
            }
        );
        Nui::listen(
            fileExplorerElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                Nui::WebApi::Console::log("fileExplorerElement changed.");
                fileExplorerElement.eventContext().delayToAfterProcessing(
                    [this, elem]()
                    {
                        tabAddMenu.modifyItemByLabel(
                            language->get("sessionFrontend", "fileExplorer"),
                            [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                            {
                                if (mi)
                                {
                                    Nui::WebApi::Console::log(
                                        "Menu item found, modifying disabled to " + std::to_string(dis)
                                    );
                                    mi->disabled = dis;
                                }
                            }
                        );
                    }
                );
            }
        );
        Nui::listen(
            operationQueueElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                Nui::WebApi::Console::log("operationQueueElement changed.");

                operationQueueElement.eventContext().delayToAfterProcessing(
                    [this, elem]()
                    {
                        tabAddMenu.modifyItemByLabel(
                            language->get("sessionFrontend", "operationQueue"),
                            [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                            {
                                if (mi)
                                    mi->disabled = dis;
                            }
                        );
                    }
                );
            }
        );
        Nui::listen(
            fileTrackingElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                fileTrackingElement.eventContext().delayToAfterProcessing(
                    [this, elem]()
                    {
                        tabAddMenu.modifyItemByLabel(
                            language->get("sessionFrontend", "fileTracking"),
                            [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                            {
                                if (mi)
                                    mi->disabled = dis;
                            }
                        );
                    }
                );
            }
        );
    }
};

int Session::tabId() const
{
    return impl_->tabId;
}

std::string Session::layoutId() const
{
    return impl_->sessionLayoutId;
}

void Session::onDrop(
    bool isLocalSide,
    std::vector<SharedData::DirectoryEntry> entries,
    std::optional<std::string> const& subdir
)
{
    if (isLocalSide)
    {
        // confirm dialog: not implemented
        Log::warn("Dropping files on local side is not implemented.");
        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "dropNotImplementedTitle"),
            .text = language->get("sessionFrontend", "dropNotImplementedText"),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "dropNotImplemented",
        });
        return;
    }

    std::vector<NuiFileExplorer::Item> items;
    items.reserve(entries.size());
    std::transform(
        std::make_move_iterator(begin(entries)),
        std::make_move_iterator(end(entries)),
        std::back_inserter(items),
        [](SharedData::DirectoryEntry const& entry)
        {
            return NuiFileExplorer::Item{entry};
        }
    );

    localSideModel().onTransfer(items, subdir);
}

auto Session::makeChannelElement() -> Nui::ElementRenderer
{
    using Nui::Elements::div; // because of the global div.

    // clang-format off
    return div{}(
        observe(impl_->frontendSessionManager),
        [this]() -> Nui::ElementRenderer {
            return div{
                style = observe(impl_->options).generate([this](){
                    return fmt::format("height: 100%; width: 100%; background-color: {};", impl_->options->theme && impl_->options->theme->background ? *impl_->options->theme->background : "#202020");
                }),
                class_ = "terminal-channel",
                reference.onMaterialize([this](Nui::val element) {
                    Log::info("Channel terminal materialized");
                    if (impl_->frontendSessionManager.value())
                    {
                        impl_->frontendSessionManager.value()->createChannel(
                            element,
                            *impl_->options,
                            std::bind(&Session::onOpenChannel, this, std::placeholders::_1, std::placeholders::_2),
                            std::bind(&Session::onChannelLoss, this, std::placeholders::_1)
                        );
                    }
                })
            }();
        }
    );
    // clang-format on
}

void Session::onChannelLoss(Ids::ChannelId const& id)
{
    Log::warn("Channel loss detected for session '{}'", impl_->initialName);

    if (impl_->isInLostConnectionState.value())
        return; // Connection loss already confirmed: keep tab open so the user can save contents.

    // Race guard: on a connection drop, the Session::*::onDisconnect signal (which sets
    // isInLostConnectionState) and sshTerminalOnExit_* both arrive from background threads.
    // Depending on thread scheduling, onDisconnect may not yet have been processed when this
    // callback fires. Defer the close decision by 100ms to let it arrive first.
    // On a normal shell exit, onDisconnect never fires, so after the delay the flag is still
    // false and the tab closes as expected.
    const std::string sessionLayoutId = impl_->sessionLayoutId;
    const std::string channelId = id.value();
    Nui::val::global("setTimeout")(
        Nui::bind(
            [this, sessionLayoutId, channelId]()
            {
                if (!impl_->isInLostConnectionState.value())
                    Nui::val::global("contentPanelManager").call<void>("closeTerminalById", sessionLayoutId, channelId);
            }
        ),
        Nui::val{100}
    );
}

NuiFileExplorer::Side& Session::remoteFileGridSide()
{
    return impl_->fileGrid.rightSide();
}
NuiFileExplorer::Side& Session::localFileGridSide()
{
    return impl_->fileGrid.leftSide();
}

auto Session::makeFileExplorerElement() -> Nui::ElementRenderer
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        style = "width: 100%; height: auto; display: block",
    }(
        div{
            style = observe(impl_->isInLostConnectionState).generate([this](){
                return fmt::format("display: {};", impl_->isInLostConnectionState.value() ? "flex" : "none");
            }),
            class_ = "session-connection-lost-overlay",
        }(
            span{}(language->getObserved("sessionFrontend", "connectionLost"))
        ),
        impl_->fileGrid()
    );
    // clang-format on
}

Session::Session(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    Persistence::SessionOptions engineOptions,
    Persistence::UiOptions uiOptions,
    std::string initialName,
    std::optional<std::string> layoutName,
    InputDialog* inputDialog,
    ConfirmDialog* confirmDialog,
    FilePropertyDialog* filePropertyDialog,
    std::function<void(Session const*)> closeSelf,
    std::function<std::string(Session const* ptr, std::string const&)> disambiguateTitle,
    bool visible,
    int tabId
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          std::move(engineOptions),
          std::move(uiOptions),
          std::move(initialName),
          std::move(layoutName),
          inputDialog,
          confirmDialog,
          filePropertyDialog,
          std::move(closeSelf),
          std::move(disambiguateTitle),
          visible,
          tabId
      )}
{
    if (std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine))
    {
        createExecutingEngine();
        // what about files?
    }
    else if (std::holds_alternative<Persistence::SshSessionOptions>(impl_->engineOptions.engine))
    {
        createSshEngine();
        setupFileGrid();
    }
    else
    {
        Log::error("Unsupported frontendSessionManager engine type");
        return;
    }

    Nui::globalEventContext.executeActiveEventsImmediately();
}

std::optional<nlohmann::json> Session::getLayout() const
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
                    {"flavor", fileGridFlavorToString(impl_->fileGrid.leftSide().flavor())},
                },
            },
            {
                "rightSide",
                {
                    {"flavor", fileGridFlavorToString(impl_->fileGrid.rightSide().flavor())},
                },
            },
        },
    }};
    return layoutObject;
}

void Session::setupFileGrid()
{
    impl_->fileGrid.onError(
        [this](auto const& message)
        {
            Log::error("File grid error: {}", message);
            impl_->confirmDialog->open({
                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                .headerText = "File Grid Error",
                .text = message,
                .buttons = ConfirmDialog::Button::Ok,
                .neverShowAgainId = "fileGridError",
            });
        }
    );

    remoteSideModel().setItemUpdateFunction(
        [this](bool sorted, bool reapplySelection)
        {
            remoteFileGridSide().updateItems(sorted, reapplySelection);
        }
    );
    localSideModel().setItemUpdateFunction(
        [this](bool sorted, bool reapplySelection)
        {
            localFileGridSide().updateItems(sorted, reapplySelection);
        }
    );
    remoteSideModel().setLocalModel(&localSideModel());
    localSideModel().setRemoteModel(&remoteSideModel());
}

void Session::saveTerminalContents(std::filesystem::path const& file, std::vector<std::string> const& contents)
{
    // Save channels to file(s):
    int indexIncrement = 0;
    const auto basePath = file.parent_path();
    const auto extension = file.extension().string();
    const auto baseName = file.stem().string();
    for (auto const& textContent : contents)
    {
        const auto filePath = basePath / fmt::format("{}_{}{}", baseName, indexIncrement++, extension);
        Nui::RpcClient::callWithBackChannel(
            "RpcFilesystem::writeFile",
            [filePath](Nui::val response)
            {
                if (!response.hasOwnProperty("success"))
                {
                    Log::error(
                        "Invalid response from RpcFilesystem::writeFile for file '{}'", filePath.generic_string()
                    );
                    return;
                }

                const auto success = response["success"].as<bool>();
                if (!success)
                {
                    const auto error = response["error"].as<std::string>();
                    Log::error("Failed to write file '{}': {}", filePath.generic_string(), error);
                    return;
                }

                Log::info("Successfully wrote file '{}'", filePath.generic_string());
            },
            filePath,
            textContent
        );
    }
}

void Session::onLockedModeUserInput(Ids::ChannelId channelId, std::string const& input)
{
    Log::info("Received user input by channel '{}' in locked mode: {}", channelId.id(), input);
    if (input == "\r" || input == "\n")
        return closeSelf();

    if (input == "s" || input == "S")
    {
        if (!impl_->frontendSessionManager.value())
            return;

        Nui::FileDialog::showSaveDialog(
            Nui::FileDialog::SaveDialogOptions{
                // all are optional
                .title = "Pick directory / file",
                .defaultPath = "%userprofile%",
                .filters = {},
                .forcePath = false,
                .forceOverwrite = false,
            },
            [this](std::optional<std::filesystem::path> const& result)
            {
                if (!result.has_value())
                {
                    Log::info("User cancelled save dialog in locked mode");
                    return;
                }

                saveTerminalContents(result.value(), impl_->savedTerminalContents);
            }
        );
        return;
    }
}

void Session::createSshEngine()
{
    Log::info("Creating SSH engine");

    impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
        std::make_unique<SshTerminalEngine>(SshTerminalEngine::Settings{
            .sessionOptions = impl_->engineOptions,
            .onConnectionLoss = std::bind(&Session::onTerminalConnectionLoss, this),
        }),
        std::bind(&Session::onLockedModeUserInput, this, std::placeholders::_1, std::placeholders::_2)
    );

    impl_->frontendSessionManager.value()->open(
        std::bind(&Session::onOpenSession, this, std::placeholders::_1, std::placeholders::_2)
    );
}

void Session::createExecutingEngine()
{
    impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
        std::make_unique<ExecutingTerminalEngine>(ExecutingTerminalEngine::Settings{
            .engineOptions = std::get<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine),
            .termios = impl_->engineOptions.termios.value(),
            .onProcessChange =
                [this](std::string const& cmdline)
            {
                Log::info("Tab title changed: {}", cmdline);
                *impl_->tabTitle = impl_->disambiguateTitle(this, cmdline);
                Nui::globalEventContext.executeActiveEventsImmediately();
            },
        }),
        std::bind(&Session::onLockedModeUserInput, this, std::placeholders::_1, std::placeholders::_2)
    );

    impl_->frontendSessionManager.value()->open(
        std::bind(&Session::onOpenSession, this, std::placeholders::_1, std::placeholders::_2)
    );
}

Session::~Session() = default;

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(Session);

void Session::openLocalFilesystem()
{
    // initial navigate to default path:
    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::getHome",
        [this](Nui::val response)
        {
            if (!response.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::getHome");
                return;
            }

            const auto success = response["success"].as<bool>();
            if (!success)
            {
                const auto error = response["error"].as<std::string>();
                Log::error("Failed to get home directory: {}", error);
                return;
            }

            if (!response.hasOwnProperty("path"))
            {
                Log::error("Invalid response from RpcFilesystem::getHome: missing 'path'");
                impl_->confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "Get Home Directory Failed",
                    .text = "Invalid response from backend: missing 'path'",
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "getHomeDirectoryFailed",
                });
                return;
            }

            const auto homePath = response["path"].as<std::string>();
            localFileGridSide().path(homePath);
        }
    );
}

void Session::openSftp(std::string const& username)
{
    if (impl_->frontendSessionManager.value() && impl_->frontendSessionManager.value()->engine().engineName() == "ssh")
    {
        auto const& opts = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine);
        if (opts.openSftpByDefault)
        {
            Log::info("Opening SFTP by default");
            auto* sshTerminalEngine = static_cast<SshTerminalEngine*>(&impl_->frontendSessionManager.value()->engine());
            auto fileEngine = std::make_shared<FileEngine>(sshTerminalEngine);
            remoteSideModel().engine(fileEngine);
            localSideModel().engine(std::move(fileEngine));
            impl_->operationQueue.activate(remoteSideModel().engine(), sshTerminalEngine->sshSessionId());
            impl_->fileTrackingPanel.activate(&impl_->operationQueue, sshTerminalEngine->sshSessionId());
            remoteSideModel().operationQueue(&impl_->operationQueue);
            remoteSideModel().setFileTracking(&impl_->fileTrackingPanel);
            localSideModel().operationQueue(&impl_->operationQueue);
            remoteFileGridSide().path(
                fmt::format(
                    fmt::runtime(opts.sftpOptions->defaultDirectory.value_or("/home/{user}").generic_string()),
                    fmt::arg("user", username)
                )
            );
            openLocalFilesystem();
        }
    }
    else
    {
        Log::info("Cannot open SFTP for non-ssh terminal");
    }
}

void Session::onOpenSession(bool success, std::string const& info)
{
    if (!success)
    {
        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "sessionCreationFailedHeader"),
            .text = fmt::format(fmt::runtime(language->get("sessionFrontend", "sessionCreationFailedText")), info),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "sessionCreationFailed",
        });
        closeSelf();
        return;
    }
    else
    {
        Log::info("Session opened successfully: {}", info);
        if (impl_->frontendSessionManager.value() &&
            impl_->frontendSessionManager.value()->engine().engineName() == "ssh")
        {
            const auto& sshSessionOptions = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine);

            Log::info("Retrieving username for tab title");
            Nui::RpcClient::callWithBackChannel(
                "RpcSystem::getUsername",
                [this, &sshSessionOptions](Nui::val response)
                {
                    std::string username = "errorName";
                    if (response.hasOwnProperty("username"))
                        username = response["username"].as<std::string>();
                    Log::info("Retrieved username: {}", username);

                    const auto user = sshSessionOptions.user.value_or(username);
                    auto host = sshSessionOptions.host;
                    const auto port = sshSessionOptions.port.value_or(22);

                    // assume ipv6 when finding ':' in host
                    if (host.find(":") != std::string::npos)
                        host = "[" + host + "]";
                    *impl_->tabTitle = impl_->disambiguateTitle(this, fmt::format("{}@{}:{}", user, host, port));

                    openSftp(user);
                }
            );
        }

        impl_->frontendSessionManager.value()->focusFirst();
        initializeLayout();
    }
}

void Session::onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info)
{
    if (!channelId)
    {
        Log::error("Failed to open channel: {}", info);

        if (!impl_->channelElements.empty())
        {
            Nui::val::global("contentPanelManager")
                .call<void>("closeTerminalByNode", impl_->sessionLayoutId, impl_->channelElements.back()->val());
            impl_->channelElements.pop_back();
        }

        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "channelCreationFailedHeader"),
            .text = fmt::format(fmt::runtime(language->get("sessionFrontend", "channelCreationFailedText")), info),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "channelCreationFailed",
        });
        return;
    }

    Log::info("Channel opened successfully: {}", channelId->value());
}

void Session::onTerminalConnectionLoss()
{
    Log::debug("onTerminalConnectionLoss");
    if (!impl_->isInLostConnectionState.value())
        onConnectionLoss();
}

void Session::onFileExplorerConnectionClose()
{
    Log::debug("onFileExplorerConnectionClose");
    if (!impl_->isInLostConnectionState.value())
        onConnectionLoss();
}

void Session::onConnectionLoss()
{
    if (impl_->isInLostConnectionState.value())
        return;

    impl_->isInLostConnectionState = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (!impl_->frontendSessionManager.value())
    {
        Log::error("Cannot write broadcast message, no frontend session manager");
        return;
    }

    impl_->frontendSessionManager.value()->connectionLossMode(true);
    impl_->frontendSessionManager.value()->forEachChannel(
        [this](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            const auto content = channel.getAllTextContent();
            impl_->savedTerminalContents.push_back(content);
            return true;
        }
    );
    impl_->frontendSessionManager.value()->broadcast(language->get("sessionFrontend", "connectionLostTerminalMessage"));
}

void Session::shutdown(std::function<void()> onShutdown)
{
    impl_->onShutdownComplete = std::move(onShutdown);
    closeSelf();
}

void Session::closeSelf()
{
    Log::info("Session::closeSelf called");
    // Immediately make page inert to prevent user interaction from this point on.
    impl_->inertEverything = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    const bool isExecutingEngine =
        std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine);

    auto closeSelfCompletion = [this]()
    {
        Log::info("Removing session layout from content panel manager.");
        Nui::val::global("contentPanelManager").call<void>("removePanel", impl_->sessionLayoutId);

        // outside shutdown
        if (impl_->onShutdownComplete)
        {
            Log::info("Session shutdown complete.");
            impl_->onShutdownComplete();
            return;
        }
        else
            impl_->closeSelf(this);
    };

    if (!isExecutingEngine && (impl_->frontendSessionManager.value() || remoteSideModel().engine()))
    {
        Log::info("Session shutdown started.");

        impl_->operationQueue.deactivate();
        impl_->fileTrackingPanel.deactivate();
        auto terminalDispose = [this, closeSelfCompletion]()
        {
            Log::info("Disposing frontend ssh manager.");
            if (impl_->frontendSessionManager.value())
            {
                impl_->frontendSessionManager.value()->dispose(
                    [closeSelfCompletion]()
                    {
                        Log::info("Session.closeSelfCompletion()");
                        closeSelfCompletion();
                    }
                );
            }
        };

        // Not necessary, because the session destruction will take care of it:
        // if (auto fileEngine = remoteSideModel().engine(); fileEngine)
        // {
        //     Log::info("Disposing file engine.");
        //     fileEngine->dispose(terminalDispose);
        //     return;
        // }
        terminalDispose();
    }
    else
    {
        Log::info("Session shutdown is already complete.");
        closeSelfCompletion();
    }
}

std::optional<std::string> Session::getProcessIdIfExecutingEngine() const
{
    if (std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine))
        return static_cast<ExecutingTerminalEngine&>(impl_->frontendSessionManager.value()->engine()).id();
    return std::nullopt;
}

std::string Session::name() const
{
    return impl_->initialName;
}

std::weak_ptr<Nui::Observed<std::string>> Session::tabTitle() const
{
    return impl_->tabTitle;
}

bool Session::visible() const
{
    return impl_->isVisible.value();
}

void Session::visible(bool value)
{
    impl_->isVisible = value;
    Nui::globalEventContext.executeActiveEventsImmediately();
    if (value)
        impl_->frontendSessionManager.value()->focusFirst();
}

auto Session::makeOperationQueueElement() -> Nui::ElementRenderer
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        style = "width: 100%; height: auto; display: block",
    }(
        div{
            style = observe(impl_->isInLostConnectionState).generate([this](){
                return fmt::format("display: {};", impl_->isInLostConnectionState.value() ? "flex" : "none");
            }),
            class_ = "session-connection-lost-overlay",
        }(
            span{}(language->getObserved("sessionFrontend", "connectionLost"))
        ),
        impl_->operationQueue()
    );
    // clang-format on
}

void Session::onChannelClosedByUser(Ids::ChannelId const& channelId)
{
    using namespace std::string_literals;

    // Removing element from vector:
    std::erase_if(
        impl_->channelElements,
        [channelId](auto elem) -> bool
        {
            const auto outerVal = elem->val();
            const auto channelEl = outerVal.template call<Nui::val>("querySelector", ".terminal-channel"s);
            if (channelEl.isNull() || channelEl.isUndefined())
                return false;
            if (!channelEl.template call<bool>("hasAttribute", "data-channelid"s))
                return false;
            return channelEl.template call<std::string>("getAttribute", "data-channelid"s) == channelId.value();
        }
    );

    // Removing channel in frontend:
    using namespace std::string_literals;
    impl_->frontendSessionManager.value()->closeChannel(channelId);
}

void Session::loadLayoutExtras(nlohmann::json const& layoutExtra)
{
    if (layoutExtra.contains("fileGrid"))
    {
        auto fileGridExtra = layoutExtra["fileGrid"];
        if (fileGridExtra.contains("leftSide") && fileGridExtra["leftSide"].contains("flavor"))
        {
            impl_->fileGrid.leftSide().flavor(
                NuiFileExplorer::fileGridFlavorFromString(fileGridExtra["leftSide"]["flavor"].get<std::string>())
            );
        }
        if (fileGridExtra.contains("rightSide") && fileGridExtra["rightSide"].contains("flavor"))
        {
            impl_->fileGrid.rightSide().flavor(
                NuiFileExplorer::fileGridFlavorFromString(fileGridExtra["rightSide"]["flavor"].get<std::string>())
            );
        }
    }
}

void Session::initializeLayout()
{
    Nui::WebApi::Console::log("Initializing session layout");

    Nui::val element;
    if (auto host = impl_->layoutHost.lock(); host)
    {
        element = host->val();
    }
    else
    {
        Log::info("Waiting for layout host");
        impl_->waitingForLayoutHost = true;
        return;
    }

    std::optional<nlohmann::json> layout = std::nullopt;

    if (impl_->layoutName != Constants::defaultLayoutName)
    {
        if (impl_->engineOptions.layouts && impl_->layoutName)
        {
            if (auto iter = impl_->engineOptions.layouts->find(*impl_->layoutName);
                iter != impl_->engineOptions.layouts->end())
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

    if (layout && layout->contains("__extra"))
        loadLayoutExtras((*layout)["__extra"]);

    auto addPanelArgument = Nui::val::object();
    addPanelArgument.set("host", element);
    addPanelArgument.set("id", impl_->sessionLayoutId);
    addPanelArgument.set("layoutString", layout ? Nui::val(layout->dump()) : Nui::val::undefined());
    addPanelArgument.set(
        "terminalFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                Nui::WebApi::Console::log("Channel factory content panel manager");
                auto elem = Nui::Dom::makeStandaloneElement(makeChannelElement());
                impl_->channelElements.push_back(elem);
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

                    onChannelClosedByUser(channelId);
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
                // OpenFileExplorer
                if (impl_->fileExplorerElement.value())
                {
                    Log::warn("There is already a file explorer, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->fileExplorerElement = Nui::Dom::makeStandaloneElement(makeFileExplorerElement());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->fileExplorerElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "fileExplorerDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                // Remove FileExplorer
                if (!impl_->fileExplorerElement.value())
                {
                    Log::warn("There is no file explorer to remove");
                    return Nui::val::undefined();
                }
                Nui::WebApi::Console::log("Removing file explorer element");
                impl_->fileExplorerElement.value().reset();
                impl_->fileExplorerElement.modifyNow();
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
                impl_->operationQueueElement = Nui::Dom::makeStandaloneElement(makeOperationQueueElement());
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
                impl_->sessionOptionsElement = Nui::Dom::makeStandaloneElement(impl_->sessionOptions());
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
                impl_->fileTrackingElement = Nui::Dom::makeStandaloneElement(impl_->fileTrackingPanel());
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
        "openAddContextMenu",
        Nui::bind(
            [this](Nui::val val)
            {
                Log::info("openAddContextMenu called");
                if (!val.isString())
                {
                    Log::error("openAddContextMenu needs to be called with a string argument");
                }
                Nui::globalEventContext.executeActiveEventsImmediately();
                impl_->tabAddMenu.openNextTo(val.as<std::string>());
            },
            std::placeholders::_1
        )
    );

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
        closeSelf();
    }
}

Nui::ElementRenderer Session::operator()()
{
    using Nui::Elements::div; // because of the global div.
    Log::info("Session::operator()");

    // clang-format off
    return div{
        class_ = observe(impl_->isVisible).generate([this]() {
            return classes("terminal-session", impl_->isVisible.value() ? "terminal-session-visible" : "terminal-session-hidden");
        }),
        !(reference = [this](
            std::weak_ptr<Nui::Dom::BasicElement>&& elem
        ){
            impl_->layoutHost = elem.lock();
            if (impl_->waitingForLayoutHost) {
                initializeLayout();
                impl_->waitingForLayoutHost = false;
            }
        }),
        "inert"_attr = observe(impl_->inertEverything).generate([this]() -> std::optional<std::string> {
            return impl_->inertEverything.value() ? "true"s : std::optional<std::string>{std::nullopt};
        })
    }(
        impl_->tabAddMenu()
    );
    // clang-format on
}

RemoteSideModel& Session::remoteSideModel()
{
    return static_cast<RemoteSideModel&>(remoteFileGridSide().model());
}
LocalSideModel& Session::localSideModel()
{
    return static_cast<LocalSideModel&>(localFileGridSide().model());
}