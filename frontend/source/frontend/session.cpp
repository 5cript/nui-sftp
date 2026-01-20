#include <persistence/state/session_options.hpp>
#include <frontend/session.hpp>
#include <frontend/terminal/frontend_ssh_manager.hpp>
#include <frontend/terminal/executing_engine.hpp>
#include <frontend/terminal/user_control_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <frontend/terminal/sftp_file_engine.hpp>
#include <frontend/classes.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/session_components/session_options.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <nui-file-explorer/file_grid.hpp>
#include <persistence/state_holder.hpp>
#include <constants/layouts.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <nui/event_system/event_context.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/utility/delocalized.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

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
    Persistence::UiOptions uiOptions;

    // (Ssh, ...)Session / FrontendSshManager Engine:
    Persistence::SessionOptions engineOptions;
    Nui::Observed<Persistence::TerminalOptions> options;

    // Dialogs:
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;

    // File Explorer Things:
    NuiFileExplorer::FileGrid fileGrid;
    std::shared_ptr<Nui::Dom::Element> fileExplorer;

    // Operation Queue for File Explorer
    OperationQueue operationQueue;
    std::shared_ptr<Nui::Dom::Element> operationQueueElement;

    // Layout Engine Related
    std::weak_ptr<Nui::Dom::BasicElement> layoutHost;
    std::optional<std::string> layoutName;
    bool waitingForLayoutHost{false};

    // Channels & FrontendSshManager Connection
    Nui::Observed<std::unique_ptr<FrontendSshManager>> terminal;
    std::vector<std::shared_ptr<Nui::Dom::Element>> channelElements;

    // Session Options
    std::shared_ptr<Nui::Dom::Element> sessionOptionsElement{};
    SessionOptions sessionOptions;

    // Shutdown:
    Nui::Observed<bool> shuttingDown{false};
    std::function<void()> onShutdownComplete{};

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        Persistence::SessionOptions engineOptions,
        Persistence::UiOptions uiOptions,
        std::string initialName,
        std::optional<std::string> layoutName,
        InputDialog* inputDialog,
        ConfirmDialog* confirmDialog,
        std::function<void(Session const*)> closeSelf,
        bool visible)
        : stateHolder{stateHolder}
        , events{events}
        , initialName{std::move(initialName)}
        , tabTitle{std::make_shared<Nui::Observed<std::string>>(this->initialName)}
        , sessionLayoutId{Nui::val::global("generateId")().as<std::string>()}
        , closeSelf{std::move(closeSelf)}
        , isVisible{visible}
        , uiOptions{uiOptions}
        , engineOptions{std::move(engineOptions)}
        , options{this->engineOptions.terminalOptions.value()}
        , inputDialog{inputDialog}
        , confirmDialog{confirmDialog}
        , fileGrid{{
              .pathBarOnTop = uiOptions.fileGridPathBarOnTop,
         },
            std::make_unique<LocalSideModel>(this->uiOptions, confirmDialog, inputDialog),
            std::make_unique<RemoteSideModel>(this->uiOptions, confirmDialog, inputDialog),
        }
        , fileExplorer{}
        , operationQueue{this->stateHolder, this->events, this->initialName, this->confirmDialog, static_cast<LocalSideModel*>(&fileGrid.leftModel()), static_cast<RemoteSideModel*>(&fileGrid.rightModel())}
        , operationQueueElement{}
        , layoutHost{}
        , layoutName{std::move(layoutName)}
        , terminal{}
        , channelElements{}
        , sessionOptionsElement{}
        , sessionOptions{stateHolder, events, this->initialName, this->sessionLayoutId, confirmDialog}
    {}
};

auto Session::makeChannelElement() -> Nui::ElementRenderer
{
    using Nui::Elements::div; // because of the global div.

    // clang-format off
    return div{}(
        observe(impl_->terminal),
        [this]() -> Nui::ElementRenderer {
            return div{
                style = "height: 100%; width: 100%",
                class_ = "terminal-channel",
                reference.onMaterialize([this](Nui::val element) {
                    Log::info("Channel terminal materialized");
                    if (impl_->terminal.value())
                    {
                        impl_->terminal.value()->createChannel(element, *impl_->options, std::bind(&Session::onOpenChannel, this, std::placeholders::_1, std::placeholders::_2));
                    }
                })
            }();
        }
    );
    // clang-format on
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
    using Nui::Elements::div; // because of the global div.
    using namespace Nui::Attributes;

    // clang-format off
    return div{
        style = "width: 100%; height: auto; display: block",
    }(
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
    std::function<void(Session const*)> closeSelf,
    bool visible
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
          std::move(closeSelf),
          visible
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
        Log::error("Unsupported terminal engine type");
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
                .state = ConfirmDialog::State::Negative,
                .headerText = "File Grid Error",
                .text = message,
                .buttons = ConfirmDialog::Button::Ok,
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

void Session::createSshEngine()
{
    Log::info("Creating SSH engine");

    impl_->terminal = std::make_unique<FrontendSshManager>(
        std::make_unique<SshTerminalEngine>(SshTerminalEngine::Settings{
            .engineOptions = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine),
            .onExit = std::bind(&Session::onTerminalConnectionClose, this),
            .onBeforeExit = std::bind(&Session::onBeforeTerminalConnectionClose, this),
        }),
        true
    );

    impl_->terminal.value()->open(
        std::bind(&Session::onOpenSession, this, std::placeholders::_1, std::placeholders::_2)
    );
}

void Session::onBeforeTerminalConnectionClose()
{
    // TODO:
    // if (impl_->terminal.value())
    // {
    //     impl_->terminal.value()->iterateAllChannels([](std::string const& /*channelId*/, TerminalChannel&
    //     channel) {
    //         std::string id = channel.stealTerminal();
    //         return true;
    //     });
    // }
}

void Session::createExecutingEngine()
{
    impl_->terminal = std::make_unique<FrontendSshManager>(
        std::make_unique<ExecutingTerminalEngine>(ExecutingTerminalEngine::Settings{
            .engineOptions = std::get<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine),
            .termios = impl_->engineOptions.termios.value(),
            .onProcessChange =
                [this](std::string const& cmdline)
            {
                Log::info("Tab title changed: {}", cmdline);
                *impl_->tabTitle = cmdline;
                Nui::globalEventContext.executeActiveEventsImmediately();
            },
        }),
        false
    );

    impl_->terminal.value()->open(
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
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "Get Home Directory Failed",
                    .text = "Invalid response from backend: missing 'path'",
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto homePath = response["path"].as<std::string>();
            localFileGridSide().path(homePath);
        }
    );
}

void Session::openSftp()
{
    if (impl_->terminal.value() && impl_->terminal.value()->engine().engineName() == "ssh")
    {
        auto const& opts = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine);
        if (opts.openSftpByDefault)
        {
            Log::info("Opening SFTP by default");
            auto* sshTerminalEngine = static_cast<SshTerminalEngine*>(&impl_->terminal.value()->engine());
            auto fileEngine = std::make_shared<SftpFileEngine>(sshTerminalEngine);
            remoteSideModel().engine(fileEngine);
            localSideModel().engine(std::move(fileEngine));
            impl_->operationQueue.activate(remoteSideModel().engine(), sshTerminalEngine->sshSessionId());
            remoteSideModel().operationQueue(&impl_->operationQueue);
            localSideModel().operationQueue(&impl_->operationQueue);
            remoteFileGridSide().path(opts.sftpOptions->defaultDirectory.value_or("/"));
            openLocalFilesystem();
        }
    }
    else
    {
        Log::info("Cannot open SFTP for non-ssh terminal");
    }
}

void Session::fallbackToUserControlEngine()
{
    // TODO:

    // impl_->terminal = std::make_unique<FrontendSshManager>(
    //     std::make_unique<UserControlEngine>(UserControlEngine::Settings{
    //         .onInput =
    //             [this](std::string const& input) {
    //                 if (input == "\u0003" && impl_->closeSelf)
    //                     impl_->closeSelf(*this);
    //             },
    //     }),
    //     false);

    // impl_->terminal.value()->open([](bool success, std::string const& info) {
    //     if (!success)
    //     {
    //         Log::error("Failed to open user control terminal: {}", info);
    //         return;
    //     }
    //     Log::info("User control terminal opened successfully");
    // });

    // impl_->terminal.value()->write(
    //     fmt::format("\033[1;31mFailed to create instance: {}.\r\nPress Ctrl+C do close this tab.\033[00m", info),
    //     false);
    // Nui::globalEventContext.executeActiveEventsImmediately();

    // New layout?:
    // initializeLayout();
}

void Session::onOpenSession(bool success, std::string const& info)
{
    if (!success)
    {
        impl_->confirmDialog->open({
            .state = ConfirmDialog::State::Negative,
            .headerText = language->get("sessionFrontend", "sessionCreationFailedHeader"),
            .text = fmt::format(fmt::runtime(language->get("sessionFrontend", "sessionCreationFailedText")), info),
            .buttons = ConfirmDialog::Button::Ok,
        });
        closeSelf();
        return;
    }
    else
    {
        Log::info("Session opened successfully: {}", info);
        if (impl_->terminal.value() && impl_->terminal.value()->engine().engineName() == "ssh")
        {
            const auto& sshSessionOptions = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine);

            // TODO: __todo_default__ is probably something that should be replaced with a proper default value.
            // Which most of the time is the user name of the user that started this program.
            const auto user = sshSessionOptions.user.value_or("__todo_default__");
            auto host = sshSessionOptions.host;
            const auto port = sshSessionOptions.port.value_or(22);

            // assume ipv6 when finding ':' in host
            if (host.find(":") != std::string::npos)
                host = "[" + host + "]";
            *impl_->tabTitle = user + "@" + host + ":" + std::to_string(port);

            openSftp();
        }

        impl_->terminal.value()->focus();
        initializeLayout();
    }
}

void Session::onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info)
{
    if (!channelId)
    {
        Log::error("Failed to open channel: {}", info);
        return;
    }

    Log::info("Channel opened successfully: {}", channelId->value());
}

void Session::onTerminalConnectionClose()
{
    // TODO:
    Log::debug("onTerminalConnectionClose");
}

void Session::onFileExplorerConnectionClose()
{
    // TODO:
    Log::debug("onFileExplorerConnectionClose");
}

void Session::shutdown(std::function<void()> onShutdown)
{
    impl_->onShutdownComplete = std::move(onShutdown);
    closeSelf();
}

void Session::closeSelf()
{
    // Immediately make page inert to prevent user interaction from this point on.
    impl_->shuttingDown = true;
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

        // "inside" shutdown (connection loss etc)
        if (impl_->closeSelf)
        {
            Log::info("Calling close self callback.");
            impl_->closeSelf(this);
            return;
        }
    };

    if (!isExecutingEngine && (impl_->terminal.value() || remoteSideModel().engine()))
    {
        Log::info("Session shutdown started.");

        impl_->operationQueue.deactivate();
        auto terminalDispose = [this, closeSelfCompletion]()
        {
            Log::info("Disposing frontend ssh manager.");
            if (impl_->terminal.value())
            {
                impl_->terminal.value()->dispose(
                    [closeSelfCompletion]()
                    {
                        Log::info("Frontend ssh manager disposed.");
                        closeSelfCompletion();
                    }
                );
            }
        };

        if (auto fileEngine = remoteSideModel().engine(); fileEngine)
        {
            Log::info("Disposing file engine.");
            fileEngine->dispose(terminalDispose);
        }
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
        return static_cast<ExecutingTerminalEngine&>(impl_->terminal.value()->engine()).id();
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
        impl_->terminal.value()->focus();
}

auto Session::makeOperationQueueElement() -> Nui::ElementRenderer
{
    return impl_->operationQueue();
}

void Session::onChannelClosedByUser(Ids::ChannelId const& channelId)
{
    using namespace std::string_literals;

    // Removing element from vector:
    std::erase_if(
        impl_->channelElements,
        [channelId](auto elem) -> bool
        {
            const auto val = elem->val();
            if (val.template call<bool>("hasAttribute", "data-channelid"s))
            {
                return val.template call<std::string>("getAttribute", "data-channelid"s) == channelId.value();
            }
            else
            {
                // FIXME: I can see this warning, which should not happen, but it does.
                Log::warn("Channel element does not have a channel id attribute");
            }
            return false;
        }
    );

    // Removing channel in frontend:
    using namespace std::string_literals;
    impl_->terminal.value()->closeChannel(channelId);
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

    Nui::val::global("contentPanelManager")
        .call<void>(
            "addPanel",
            element,
            impl_->sessionLayoutId,
            layout ? layout->dump() : "",
            Nui::bind(
                [this]() -> Nui::val
                {
                    Nui::WebApi::Console::log("Channel factory content panel manager");
                    auto elem = Nui::Dom::makeStandaloneElement(makeChannelElement());
                    impl_->channelElements.push_back(elem);
                    return elem->val();
                }
            ),
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
            ),
            Nui::bind(
                [this]() -> Nui::val
                {
                    // OpenFileExplorer
                    if (impl_->fileExplorer)
                    {
                        Log::warn("There is already a file explorer, cannot open another one");
                        return Nui::val::undefined();
                    }
                    impl_->fileExplorer = Nui::Dom::makeStandaloneElement(makeFileExplorerElement());
                    return impl_->fileExplorer->val();
                }
            ),
            Nui::bind(
                [this]() -> Nui::val
                {
                    // Remove FileExplorer
                    if (!impl_->fileExplorer)
                    {
                        Log::warn("There is no file explorer to remove");
                        return Nui::val::undefined();
                    }
                    impl_->fileExplorer.reset();
                    return Nui::val::undefined();
                }
            ),
            Nui::bind(
                [this]() -> Nui::val
                {
                    if (impl_->operationQueueElement)
                    {
                        Log::warn("There is already an operation queue, cannot open another one");
                        return Nui::val::undefined();
                    }
                    impl_->operationQueueElement = Nui::Dom::makeStandaloneElement(makeOperationQueueElement());
                    return impl_->operationQueueElement->val();
                }
            ),
            Nui::bind(
                [this]() -> Nui::val
                {
                    if (!impl_->operationQueueElement)
                    {
                        Log::warn("There is no operation queue to remove");
                        return Nui::val::undefined();
                    }
                    impl_->operationQueueElement.reset();
                    return Nui::val::undefined();
                }
            ),
            Nui::bind(
                [this]() -> Nui::val
                {
                    if (impl_->sessionOptionsElement)
                    {
                        Log::warn("There are already session options, cannot open another one");
                        return Nui::val::undefined();
                    }
                    impl_->sessionOptionsElement = Nui::Dom::makeStandaloneElement(impl_->sessionOptions());
                    return impl_->sessionOptionsElement->val();
                }
            ),
            Nui::bind(
                [this]() -> Nui::val
                {
                    if (!impl_->sessionOptionsElement)
                    {
                        Log::warn("There are no session options to remove");
                        return Nui::val::undefined();
                    }
                    impl_->sessionOptionsElement.reset();
                    return Nui::val::undefined();
                }
            )
        );
}

Nui::ElementRenderer Session::operator()(bool visible)
{
    using Nui::Elements::div; // because of the global div.
    Log::info("Session::operator()");

    impl_->isVisible = visible;

    // clang-format off
    return div{
        class_ = observe(impl_->isVisible).generate([this]() {
            return classes("terminal-session", impl_->isVisible.value() ? "terminal-session-visible" : "terminal-session-hidden");
        }),
        style = Style{
            "background-color"_style = observe(impl_->options).generate([this]() -> std::string {
                if (impl_->options->theme && impl_->options->theme->background)
                    return *impl_->options->theme->background;
                return "#202020";
            }),
        },
        !(reference = [this](
            std::weak_ptr<Nui::Dom::BasicElement>&& elem
        ){
            impl_->layoutHost = elem.lock();
            if (impl_->waitingForLayoutHost) {
                initializeLayout();
                impl_->waitingForLayoutHost = false;
            }
        }),
        "inert"_attr = observe(impl_->shuttingDown).generate([this]() -> std::optional<std::string> {
            return impl_->shuttingDown.value() ? "true"s : std::optional<std::string>{std::nullopt};
        })
    }(
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