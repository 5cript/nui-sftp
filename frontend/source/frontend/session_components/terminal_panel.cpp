#include <frontend/session_components/terminal_panel.hpp>

#include <frontend/session/session_helpers.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <persistence/state_holder.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>

#include <ui5-sap-icons/icons/save.hpp>
#include <ui5-sap-icons/icons/copy.hpp>
#include <ui5-sap-icons/icons/document-text.hpp>

#include <fmt/format.h>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/filesystem/file_dialog.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/val.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

struct TerminalPanel::Implementation
{
    Persistence::StateHolder* stateHolder;
    ConfirmDialog* confirmDialog;
    Nui::Observed<std::unique_ptr<FrontendSessionManager>>* frontendSessionManager;
    Persistence::SessionOptions const* engineOptions;
    std::string sessionLayoutId;

    std::function<bool()> isInLostConnectionState;
    std::function<void()> onReconnectRequested;
    std::function<void()> onCloseSelfRequested;
    std::function<void(std::optional<Ids::ChannelId>, std::string const&)> onChannelOpened;

    Nui::Observed<Persistence::TerminalOptions> options;
    std::vector<std::shared_ptr<Nui::Dom::Element>> channelElements;
    std::unordered_map<Ids::ChannelId, LocalShellMeta, Ids::IdHash> localShellMeta;
    std::vector<std::string> savedTerminalContents;

    explicit Implementation(Params&& params)
        : stateHolder{params.stateHolder}
        , confirmDialog{params.confirmDialog}
        , frontendSessionManager{params.frontendSessionManager}
        , engineOptions{params.engineOptions}
        , sessionLayoutId{std::move(params.sessionLayoutId)}
        , isInLostConnectionState{std::move(params.isInLostConnectionState)}
        , onReconnectRequested{std::move(params.onReconnectRequested)}
        , onCloseSelfRequested{std::move(params.onCloseSelfRequested)}
        , onChannelOpened{std::move(params.onChannelOpened)}
        , options{engineOptions->terminalOptions.value()}
    {}
};

namespace
{
    /**
     * @brief Renders the per-terminal toolbar row (identity icon on the left,
     *        save / copy / copy-plain action buttons on the right).  Shared by
     *        all three channel factories.
     */
    Nui::ElementRenderer renderTerminalToolbar(
        TerminalPanel& self,
        Nui::ElementRenderer identityIcon,
        std::shared_ptr<std::optional<Ids::ChannelId>> channelIdCell,
        std::string const& terminalBackgroundColor
    )
    {
        namespace Snc = ScriptNuiComponents;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // Darken the xterm background 18% for a distinct toolbar strip that
        // works on light and dark themes.
        const std::string barBg = terminalBackgroundColor.empty()
            ? std::string{"#1a1a1a"}
            : fmt::format("color-mix(in srgb, {} 82%, black 18%)", terminalBackgroundColor);

        return div{
            class_ = "terminal-toolbar",
            style = fmt::format("background-color: {};", barBg),
        }(
            span{class_ = "terminal-toolbar__identity"}(std::move(identityIcon)),
            span{class_ = "terminal-toolbar__spacer"}(),
            span{class_ = "terminal-toolbar__actions"}(
                Snc::button({
                    .icon = Ui5Icons::save(),
                    .attributes = {
                        Nui::Attributes::title = std::string{"Save terminal to file"},
                        onClick = [&self, channelIdCell](Nui::WebApi::MouseEvent e) {
                            e.stopPropagation();
                            if (channelIdCell && *channelIdCell)
                                self.saveChannelToFile(**channelIdCell);
                        },
                    },
                    .styleVariant = Snc::StyleVariant::Transparent,
                }),
                Snc::button({
                    .icon = Ui5Icons::copy(),
                    .attributes = {
                        Nui::Attributes::title = std::string{"Copy terminal to clipboard (with formatting)"},
                        onClick = [&self, channelIdCell](Nui::WebApi::MouseEvent e) {
                            e.stopPropagation();
                            if (channelIdCell && *channelIdCell)
                                self.copyChannelToClipboard(**channelIdCell);
                        },
                    },
                    .styleVariant = Snc::StyleVariant::Transparent,
                }),
                Snc::button({
                    .icon = Ui5Icons::document_text(),
                    .attributes = {
                        Nui::Attributes::title = std::string{"Copy terminal as plain text (strip ANSI/control codes)"},
                        onClick = [&self, channelIdCell](Nui::WebApi::MouseEvent e) {
                            e.stopPropagation();
                            if (channelIdCell && *channelIdCell)
                                self.copyChannelToClipboardPlain(**channelIdCell);
                        },
                    },
                    .styleVariant = Snc::StyleVariant::Transparent,
                })
            )
        );
    }

    /**
     * @brief Writes contents[i] to "{stem}_{i}{ext}" alongside @p file for each
     *        entry.  Used by the locked-mode S-key save-all path.
     */
    void saveTerminalContents(
        std::filesystem::path const& file,
        std::vector<std::string> const& contents
    )
    {
        int indexIncrement = 0;
        const auto basePath = file.parent_path();
        const auto extension = file.extension().string();
        const auto baseName = file.stem().string();
        for (auto const& textContent : contents)
        {
            const auto filePath =
                basePath / fmt::format("{}_{}{}", baseName, indexIncrement++, extension);
            SessionInternal::writeChannelContentToFile(filePath, textContent);
        }
    }
}

TerminalPanel::TerminalPanel(Params params)
    : impl_{std::make_unique<Implementation>(std::move(params))}
{}
TerminalPanel::~TerminalPanel() = default;
TerminalPanel::TerminalPanel(TerminalPanel&&) = default;
TerminalPanel& TerminalPanel::operator=(TerminalPanel&&) = default;

Nui::ElementRenderer TerminalPanel::makeChannelElement()
{
    using Nui::Elements::div;

    // clang-format off
    return div{}(
        observe(*impl_->frontendSessionManager),
        [this]() -> Nui::ElementRenderer {
            const bool isLocalShellEngine =
                std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions->engine);
            const std::string terminalBackgroundColor =
                (impl_->options->theme && impl_->options->theme->background)
                    ? *impl_->options->theme->background
                    : std::string{};
            // Shared cell that the toolbar closures read on click and that
            // onOpenChannel fills once the channel is actually created.
            auto channelIdCell = std::make_shared<std::optional<Ids::ChannelId>>(std::nullopt);

            return div{
                style = "height: 100%; width: 100%; display: flex; flex-direction: column;",
            }(
                renderTerminalToolbar(
                    *this,
                    SessionInternal::resolveIdentityIcon(impl_->engineOptions->icon, isLocalShellEngine),
                    channelIdCell,
                    terminalBackgroundColor
                ),
                div{
                    style = observe(impl_->options).generate([this](){
                        return fmt::format(
                            "flex: 1 1 auto; min-height: 0; width: 100%; background-color: {};",
                            impl_->options->theme && impl_->options->theme->background
                                ? *impl_->options->theme->background
                                : "#202020"
                        );
                    }),
                    class_ = "terminal-channel",
                    reference.onMaterialize([this, channelIdCell](Nui::val element) {
                        Log::info("Channel terminal materialized");
                        if (!impl_->frontendSessionManager->value())
                            return;

                        auto onCreated = [this, channelIdCell](
                            std::optional<Ids::ChannelId> chId, std::string const& info
                        ) {
                            if (chId)
                                *channelIdCell = chId;
                            if (impl_->onChannelOpened)
                                impl_->onChannelOpened(chId, info);
                        };

                        // Engine-specific per-call options: shell sessions spawn a process;
                        // SSH sessions carry no per-call options (state lives in the engine).
                        if (auto const* execOpts = std::get_if<Persistence::ExecutingSessionOptions>(&impl_->engineOptions->engine))
                        {
                            ExecutingChannelCreationOptions creationOptions;
                            creationOptions.executingOptions = *execOpts;
                            creationOptions.termios = impl_->engineOptions->termios.value();
                            impl_->frontendSessionManager->value()->createChannel(
                                element,
                                *impl_->options,
                                creationOptions,
                                onCreated,
                                [this](Ids::ChannelId const& id) { onChannelLoss(id); }
                            );
                        }
                        else
                        {
                            ChannelCreationOptions creationOptions{};
                            impl_->frontendSessionManager->value()->createChannel(
                                element,
                                *impl_->options,
                                creationOptions,
                                onCreated,
                                [this](Ids::ChannelId const& id) { onChannelLoss(id); }
                            );
                        }
                    })
                }()
            );
        }
    );
    // clang-format on
}

Nui::ElementRenderer TerminalPanel::makeLocalShellChannelElement(std::string const& shellName)
{
    using Nui::Elements::div;

    // Resolve the saved shell's settings up-front. fullyResolve() fills in
    // Referenceable values (termios, terminalOptions, ...) from the top-level
    // state maps; the raw stateCache() leaves them default-constructed (for
    // termios that means every flag bit reads as cleared).
    auto state = impl_->stateHolder->stateCache().fullyResolve();
    auto iter = state.sessions.find(shellName);
    const bool found = (iter != state.sessions.end())
        && std::holds_alternative<Persistence::ExecutingSessionOptions>(iter->second.engine);
    if (!found)
    {
        Log::error("No saved shell named '{}' (or not a shell-type session)", shellName);
        // Emit a harmless placeholder contentPanelManager will keep the tab
        // frame but nothing spawns. The session itself is unaffected.
        return div{}();
    }

    // Capture the shell's settings by value: background color for the outer
    // wrapper, terminal options and termios for the channel itself. Non-reactive
    // to future settings changes, but the tab is short-lived (one shell process)
    // so a fresh open picks up any edits the user has made in between.
    const auto terminalOptions = iter->second.terminalOptions.value();
    const auto termios = iter->second.termios.value();
    const auto execOpts = std::get<Persistence::ExecutingSessionOptions>(iter->second.engine);
    const std::string backgroundColor =
        (terminalOptions.theme && terminalOptions.theme->background)
            ? *terminalOptions.theme->background
            : std::string{"#202020"};
    // The identity icon on the toolbar inherits the shell config's own icon
    // (matches the one shown in the + tab menu).  Captured here at open time.
    const std::string identityIconName = iter->second.icon;

    // clang-format off
    return div{}(
        observe(*impl_->frontendSessionManager),
        [this, shellName, terminalOptions, termios, execOpts, backgroundColor, identityIconName]() -> Nui::ElementRenderer {
            auto channelIdCell = std::make_shared<std::optional<Ids::ChannelId>>(std::nullopt);
            return div{
                style = "height: 100%; width: 100%; display: flex; flex-direction: column;",
            }(
                renderTerminalToolbar(
                    *this,
                    SessionInternal::resolveIdentityIcon(identityIconName, /*isLocalShell=*/true),
                    channelIdCell,
                    backgroundColor
                ),
                div{
                    style = fmt::format("flex: 1 1 auto; min-height: 0; width: 100%; background-color: {};", backgroundColor),
                    class_ = "terminal-channel",
                    reference.onMaterialize([this, shellName, terminalOptions, termios, execOpts, channelIdCell](Nui::val element) {
                        Log::info("Local-shell channel terminal materialized");
                        if (!impl_->frontendSessionManager->value())
                            return;

                        // Rename only the Lumino tab (not the outer session title)
                        // and keep LocalShellMeta.cmdline fresh for captureSnapshot.
                        auto onProcessChange = [this](Ids::ChannelId const& channelId, std::string const& cmdline)
                        {
                            if (auto metaIter = impl_->localShellMeta.find(channelId);
                                metaIter != impl_->localShellMeta.end())
                                metaIter->second.cmdline = cmdline;

                            Nui::val::global("contentPanelManager")
                                .call<void>("renameTerminalById", impl_->sessionLayoutId, channelId.value(), cmdline);
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        };

                        // Record shell metadata under the assigned ChannelId so
                        // captureSnapshot can build a LocalShellAdoption.  The
                        // captured settings snapshot the spawn-time values.
                        auto onCreated = [this, shellName, terminalOptions, termios, execOpts, channelIdCell](
                                             std::optional<Ids::ChannelId> const& channelId,
                                             std::string const& info)
                        {
                            if (channelId)
                            {
                                *channelIdCell = channelId;
                                LocalShellMeta meta{
                                    .shellConfigName = shellName,
                                    .cmdline = {},
                                    .terminalOptions = terminalOptions,
                                    .termios = termios,
                                    .execOpts = execOpts,
                                };
                                impl_->localShellMeta.emplace(*channelId, std::move(meta));
                            }
                            if (impl_->onChannelOpened)
                                impl_->onChannelOpened(channelId, info);
                        };

                        // Shell-specific terminalOptions its colour theme, font,
                        // cursor style etc. make each local-shell visibly distinguishable
                        // from the SSH terminal it sits next to.
                        impl_->frontendSessionManager->value()->createLocalShellChannel(
                            element,
                            terminalOptions,
                            execOpts,
                            termios,
                            std::move(onProcessChange),
                            std::move(onCreated),
                            [this](Ids::ChannelId const& id) { onChannelLoss(id); }
                        );
                    })
                }()
            );
        }
    );
    // clang-format on
}

Nui::ElementRenderer TerminalPanel::makeAdoptedLocalShellChannelElement(LocalShellAdoption adoption)
{
    using Nui::Elements::div;

    const std::string backgroundColor =
        (adoption.terminalOptions.theme && adoption.terminalOptions.theme->background)
            ? *adoption.terminalOptions.theme->background
            : std::string{"#202020"};
    // Resolve the identity icon by looking up the shell config in persistence,
    // so the adopted terminal shows the same glyph the user configured.
    // Absent config (renamed / deleted since the snapshot was captured) is
    // fine resolveIdentityIcon falls back to a generic local-shell glyph.
    std::string identityIconName;
    {
        auto state = impl_->stateHolder->stateCache().fullyResolve();
        if (auto configIter = state.sessions.find(adoption.shellConfigName);
            configIter != state.sessions.end())
        {
            identityIconName = configIter->second.icon;
        }
    }

    // clang-format off
    return div{}(
        observe(*impl_->frontendSessionManager),
        [this, adoption = std::move(adoption), backgroundColor, identityIconName]() -> Nui::ElementRenderer {
            // Process id is known up-front seed the cell so toolbar clicks
            // work the instant the element materializes.
            auto channelIdCell = std::make_shared<std::optional<Ids::ChannelId>>(adoption.processId);
            return div{
                style = "height: 100%; width: 100%; display: flex; flex-direction: column;",
            }(
                renderTerminalToolbar(
                    *this,
                    SessionInternal::resolveIdentityIcon(identityIconName, /*isLocalShell=*/true),
                    channelIdCell,
                    backgroundColor
                ),
                div{
                    style = fmt::format("flex: 1 1 auto; min-height: 0; width: 100%; background-color: {};", backgroundColor),
                    class_ = "terminal-channel",
                    reference.onMaterialize([this, adoption](Nui::val element) {
                        Log::info("Adopted local-shell channel terminal materialized");
                        if (!impl_->frontendSessionManager->value())
                            return;

                        auto onProcessChange = [this](Ids::ChannelId const& channelId, std::string const& cmdline)
                        {
                            if (auto metaIter = impl_->localShellMeta.find(channelId);
                                metaIter != impl_->localShellMeta.end())
                                metaIter->second.cmdline = cmdline;

                            Nui::val::global("contentPanelManager")
                                .call<void>("renameTerminalById", impl_->sessionLayoutId, channelId.value(), cmdline);
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        };

                        // Re-register the meta for this process id so captureSnapshot
                        // works on a subsequent reconnect.  cmdline is carried over
                        // from the snapshot; the process hasn't forked anything new
                        // during the disconnect window.
                        auto onCreated = [this, adoption](
                                             std::optional<Ids::ChannelId> const& channelId,
                                             std::string const& info)
                        {
                            if (channelId)
                            {
                                LocalShellMeta meta{
                                    .shellConfigName = adoption.shellConfigName,
                                    .cmdline = adoption.cmdline,
                                    .terminalOptions = adoption.terminalOptions,
                                    .termios = adoption.termios,
                                    .execOpts = adoption.execOpts,
                                };
                                impl_->localShellMeta.emplace(*channelId, std::move(meta));
                            }
                            if (impl_->onChannelOpened)
                                impl_->onChannelOpened(channelId, info);
                        };

                        impl_->frontendSessionManager->value()->adoptLocalShellChannel(
                            element,
                            adoption,
                            std::move(onProcessChange),
                            std::move(onCreated),
                            [this](Ids::ChannelId const& id) { onChannelLoss(id); }
                        );
                    })
                }()
            );
        }
    );
    // clang-format on
}

void TerminalPanel::saveChannelToFile(Ids::ChannelId const& channelId)
{
    if (!impl_->frontendSessionManager->value())
        return;

    auto* channel = impl_->frontendSessionManager->value()->channel(channelId);
    if (!channel)
    {
        Log::warn("saveChannelToFile: no channel '{}'", channelId.value());
        return;
    }

    // Capture the dump synchronously the dialog is async and by the time the
    // user picks a path the xterm may have scrolled further, which is fine but
    // we want the save to reflect what was on screen at click time.
    auto dump = channel->getAllTextContent();

    Nui::FileDialog::showSaveDialog(
        Nui::FileDialog::SaveDialogOptions{
            .title = "Save terminal contents",
            .defaultPath = "%userprofile%",
            .filters = {},
            .forcePath = false,
            .forceOverwrite = false,
        },
        [dump = std::move(dump)](std::optional<std::filesystem::path> const& result) mutable
        {
            if (!result.has_value())
            {
                Log::info("User cancelled save-terminal dialog");
                return;
            }
            SessionInternal::writeChannelContentToFile(*result, dump);
        }
    );
}

void TerminalPanel::copyChannelToClipboard(Ids::ChannelId const& channelId)
{
    if (!impl_->frontendSessionManager->value())
        return;

    auto* channel = impl_->frontendSessionManager->value()->channel(channelId);
    if (!channel)
    {
        Log::warn("copyChannelToClipboard: no channel '{}'", channelId.value());
        return;
    }

    const auto dump = channel->getAllTextContent();
    // See static/source/index.js the shim wraps navigator.clipboard.writeText
    // and logs its own errors on the JS side.  Fire-and-forget here.
    Nui::val::global("writeTerminalDumpToClipboard")
        .call<void>("call", Nui::val::global("globalThis"), dump);
}

void TerminalPanel::copyChannelToClipboardPlain(Ids::ChannelId const& channelId)
{
    if (!impl_->frontendSessionManager->value())
        return;

    auto* channel = impl_->frontendSessionManager->value()->channel(channelId);
    if (!channel)
    {
        Log::warn("copyChannelToClipboardPlain: no channel '{}'", channelId.value());
        return;
    }

    // ANSI/control-byte stripping happens on the JS side.
    const auto dump = channel->getAllTextContent();
    Nui::val::global("writeTerminalPlainToClipboard")
        .call<void>("call", Nui::val::global("globalThis"), dump);
}

void TerminalPanel::onLockedModeUserInput(Ids::ChannelId channelId, std::string const& input)
{
    Log::info("Received user input by channel '{}' in locked mode: {}", channelId.id(), input);
    if (input == "\r" || input == "\n")
    {
        if (impl_->onCloseSelfRequested)
            impl_->onCloseSelfRequested();
        return;
    }

    if (input == "r" || input == "R")
    {
        if (impl_->onReconnectRequested)
            impl_->onReconnectRequested();
        return;
    }

    if (input == "s" || input == "S")
    {
        if (!impl_->frontendSessionManager->value())
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

void TerminalPanel::captureChannelContentsForLockedMode()
{
    if (!impl_->frontendSessionManager->value())
        return;
    impl_->frontendSessionManager->value()->forEachChannel(
        FrontendSessionManager::EngineFilter::PrimaryOnly,
        [this](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            const auto content = channel.getAllTextContent();
            impl_->savedTerminalContents.push_back(content);
            return true;
        }
    );
}

void TerminalPanel::onChannelCreationFailed(std::string const& info)
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
}

void TerminalPanel::onChannelLoss(Ids::ChannelId const& id)
{
    Log::warn("Channel loss detected for channel '{}'", id.value());

    // Local-shell death is a process exit, not transport loss: close
    // unconditionally.  For SSH channels, isInLostConnectionState gates it.
    const bool isLocalShell = impl_->frontendSessionManager->value() &&
        impl_->frontendSessionManager->value()->isLocalShellChannel(id);

    impl_->localShellMeta.erase(id);

    const bool inLostConnection = impl_->isInLostConnectionState && impl_->isInLostConnectionState();
    if (!isLocalShell && inLostConnection)
        return; // SSH transport already lost; keep tab open for save.

    // 100ms debounce: onDisconnect and sshTerminalOnExit race across threads;
    // letting onDisconnect land first lets the above guard fire correctly.
    const std::string sessionLayoutId = impl_->sessionLayoutId;
    const std::string channelId = id.value();
    Nui::val::global("setTimeout")(
        Nui::bind(
            [this, sessionLayoutId, channelId, isLocalShell]()
            {
                const bool stillLost = impl_->isInLostConnectionState && impl_->isInLostConnectionState();
                if (isLocalShell || !stillLost)
                    Nui::val::global("contentPanelManager").call<void>("closeTerminalById", sessionLayoutId, channelId);
            }
        ),
        Nui::val{100}
    );
}

void TerminalPanel::onChannelClosedByUser(Ids::ChannelId const& channelId)
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
    if (impl_->frontendSessionManager->value())
        impl_->frontendSessionManager->value()->closeChannel(channelId);
    impl_->localShellMeta.erase(channelId);
}

std::vector<std::shared_ptr<Nui::Dom::Element>>& TerminalPanel::channelElements()
{
    return impl_->channelElements;
}

TerminalPanel::LocalShellMeta const*
TerminalPanel::findLocalShellMeta(Ids::ChannelId const& channelId) const
{
    auto iter = impl_->localShellMeta.find(channelId);
    return iter == impl_->localShellMeta.end() ? nullptr : &iter->second;
}

void TerminalPanel::clearLocalShellMeta()
{
    impl_->localShellMeta.clear();
}
