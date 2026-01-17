#include <frontend/settings.hpp>

#include <frontend/components/icon_panel.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/classes.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <frontend/settings/termios_settings.hpp>
#include <frontend/settings/queue_options.hpp>
#include <frontend/settings/general_settings.hpp>
#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/sftp_options.hpp>
#include <frontend/settings/terminal_options.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/map_setting.hpp>
#include <frontend/settings/number_setting.hpp>
#include <frontend/settings/color_setting.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/button.hpp>
#include <ui5/components/switch.hpp>
#include <ui5/components/busy_indicator.hpp>
#include <ui5/components/message_strip.hpp>

#include <nui/frontend/api/throttle.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

using namespace std::string_literals;

struct Settings::Implementation
{
    struct CollapsibleStates
    {
        Nui::Observed<bool> localization{false};
        Nui::Observed<bool> loggingAndErrorReporting{false};
        Nui::Observed<bool> userInterface{false};
        Nui::Observed<bool> localFilesystemOptions{false};
        Nui::Observed<bool> sshOptions{false};
        Nui::Observed<bool> sftpOptions{false};
        Nui::Observed<bool> termios{false};
        Nui::Observed<bool> terminalOptions{false};
        Nui::Observed<bool> queueOptions{false};
    } collapsibleStates{};

    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;
    NewSessionDialog newSessionDialog{"settings"};
    Nui::ThrottledFunction throttledSave{};
    Nui::Observed<Settings::Section> activeSection{Settings::Section::GeneralSettings};
    Nui::Observed<std::optional<std::string>> activeSession{};
    Nui::Observed<bool> saveInProgress{false};

    Nui::Observed<std::vector<Settings::SectionSelectorOptions>> sessionSelectors{};

    GeneralSettings generalSettings;
    TermiosSettings termiosSettings;
    SshOptions sshOptions;
    SftpOptions sftpOptions;
    TerminalOptions terminalOptions;
    QueueOptions queueOptions;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* inputDialog,
        ConfirmDialog* confirmDialog,
        std::invocable auto const& onChange
    )
        : stateHolder{stateHolder}
        , events{events}
        , inputDialog{inputDialog}
        , confirmDialog{confirmDialog}
        , generalSettings{onChange, events}
        , termiosSettings{onChange}
        , sshOptions{onChange}
        , sftpOptions{onChange}
        , terminalOptions{onChange}
        , queueOptions{onChange}
    {}
};

Settings::Settings(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    InputDialog* inputDialog,
    ConfirmDialog* confirmDialog
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          inputDialog,
          confirmDialog,
          [this]()
          {
              if (impl_->throttledSave.valid())
                  impl_->throttledSave();
          }
      )}
{
    Nui::throttle(
        500,
        [this]()
        {
            onChange();
        },
        [this](Nui::ThrottledFunction&& func)
        {
            impl_->throttledSave = std::move(func);
        },
        true
    );

    listen(
        impl_->events->settingsOpen,
        [this](bool open)
        {
            if (open)
                applySettingsToUi();
        }
    );
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Settings);

void Settings::applySettingsToState(Persistence::State& state)
{
    // Uncategorized / General:
    state.logLevel = impl_->generalSettings.logLevel.value();

    // Localization Options:
    state.localizationOptions.languageCode = impl_->generalSettings.localization.language.value();
    state.localizationOptions.dateTimeFormatString = impl_->generalSettings.localization.dateTimeFormat.value();

    // Ui Options
    state.uiOptions.fileGridPathBarOnTop = impl_->generalSettings.userInterface.fileGridPathBarOnTop.value();
    state.uiOptions.fileGridExtensionIcons = impl_->generalSettings.userInterface.fileGridExtensionIcons.value();

    // Local FS
    state.localFilesystemOptions.preventDeletion =
        impl_->generalSettings.localFilesystemOptions.preventDeletion.value();
    state.localFilesystemOptions.preventRename = impl_->generalSettings.localFilesystemOptions.preventRename.value();
    state.localFilesystemOptions.preventCreateFile =
        impl_->generalSettings.localFilesystemOptions.preventCreateFile.value();
    state.localFilesystemOptions.preventCreateDirectory =
        impl_->generalSettings.localFilesystemOptions.preventCreateDirectory.value();
    state.localFilesystemOptions.homeOverride = impl_->generalSettings.localFilesystemOptions.homeOverride.value();

    // Termios
    applyTermiosSettingsToStateByKey(impl_->termiosSettings.groupKey.value(), state);

    // SSH Options:
    applySshSettingsToStateByKey(impl_->sshOptions.groupKey.value(), state);

    // SftpOptions:
    applySftpOptionsToStateByKey(impl_->sftpOptions.groupKey.value(), state);

    // Terminal Options
    applyTerminalOptionsToStateByKey(impl_->terminalOptions.groupKey.value(), state);

    // Queue Options:
    applyQueueOptionsToStateByKey(impl_->queueOptions.groupKey.value(), state);
}

void Settings::applySettingsToUi()
{
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this](bool success, Persistence::State const&)
        {
            if (!success)
                return;

            impl_->sessionSelectors.value().clear();
            for (auto const& [sessionId, session] : impl_->stateHolder->stateCache().sessions)
            {
                impl_->sessionSelectors.value().push_back(
                    Settings::SectionSelectorOptions{
                        .thisSection = Settings::Section::Session,
                        .sessionId = sessionId,
                        .icon = session.icon.value_or("it-system"),
                    }
                );
            }

            impl_->generalSettings.logLevel.value(impl_->stateHolder->stateCache().logLevel);
            impl_->generalSettings.localization.language.value(
                impl_->stateHolder->stateCache().localizationOptions.languageCode
            );
            impl_->generalSettings.localization.dateTimeFormat.value(
                impl_->stateHolder->stateCache().localizationOptions.dateTimeFormatString
            );
            impl_->generalSettings.userInterface.fileGridPathBarOnTop.value(
                impl_->stateHolder->stateCache().uiOptions.fileGridPathBarOnTop
            );
            impl_->generalSettings.userInterface.fileGridExtensionIcons.value(
                impl_->stateHolder->stateCache().uiOptions.fileGridExtensionIcons
            );
            impl_->generalSettings.localFilesystemOptions.preventDeletion.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventDeletion
            );
            impl_->generalSettings.localFilesystemOptions.preventRename.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventRename
            );
            impl_->generalSettings.localFilesystemOptions.preventCreateFile.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventCreateFile
            );
            impl_->generalSettings.localFilesystemOptions.preventCreateDirectory.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventCreateDirectory
            );
            impl_->generalSettings.localFilesystemOptions.homeOverride.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.homeOverride.value_or("")
            );

            const auto initialKey = [](auto const& map)
            {
                if (map.empty())
                    return "default"s;
                auto findDefault = map.find("default");
                if (findDefault != map.end())
                    return "default"s;
                return map.begin()->first;
            };
            const auto groupKeys = [](auto const& map)
            {
                std::vector<std::string> keys;
                keys.reserve(map.size());
                for (auto const& [key, _] : map)
                    keys.push_back(key);
                return keys;
            };
            impl_->termiosSettings.groupKey = initialKey(impl_->stateHolder->stateCache().termios);
            impl_->termiosSettings.groupKeys = groupKeys(impl_->stateHolder->stateCache().termios);
            loadTermiosSettingsFromStateByKey(
                impl_->termiosSettings.groupKey.value(), impl_->stateHolder->stateCache()
            );

            impl_->sshOptions.groupKey = initialKey(impl_->stateHolder->stateCache().sshOptions);
            impl_->sshOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().sshOptions);
            loadSshSettingsFromStateByKey(impl_->sshOptions.groupKey.value(), impl_->stateHolder->stateCache());

            impl_->sftpOptions.groupKey = initialKey(impl_->stateHolder->stateCache().sftpOptions);
            impl_->sftpOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().sftpOptions);
            loadSftpOptionsFromStateByKey(impl_->sftpOptions.groupKey.value(), impl_->stateHolder->stateCache());

            impl_->terminalOptions.groupKey = initialKey(impl_->stateHolder->stateCache().terminalOptions);
            impl_->terminalOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().terminalOptions);
            loadTerminalOptionsFromStateByKey(
                impl_->terminalOptions.groupKey.value(), impl_->stateHolder->stateCache()
            );

            impl_->queueOptions.groupKey = initialKey(impl_->stateHolder->stateCache().queueOptions);
            impl_->queueOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().queueOptions);
            loadQueueOptionsFromStateByKey(impl_->queueOptions.groupKey.value(), impl_->stateHolder->stateCache());

            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void Settings::loadTermiosSettingsFromStateByKey(std::string const& key, Persistence::State const& state)
{
    if (!state.termios.contains(key))
        return;

    auto const& termiosState = state.termios.at(key);

    impl_->termiosSettings.inputFlags.IGNBRK.value(termiosState.inputFlags.IGNBRK_);
    impl_->termiosSettings.inputFlags.BRKINT.value(termiosState.inputFlags.BRKINT_);
    impl_->termiosSettings.inputFlags.IGNPAR.value(termiosState.inputFlags.IGNPAR_);
    impl_->termiosSettings.inputFlags.PARMRK.value(termiosState.inputFlags.PARMRK_);
    impl_->termiosSettings.inputFlags.INPCK.value(termiosState.inputFlags.INPCK_);
    impl_->termiosSettings.inputFlags.ISTRIP.value(termiosState.inputFlags.ISTRIP_);
    impl_->termiosSettings.inputFlags.INLCR.value(termiosState.inputFlags.INLCR_);
    impl_->termiosSettings.inputFlags.IGNCR.value(termiosState.inputFlags.IGNCR_);
    impl_->termiosSettings.inputFlags.ICRNL.value(termiosState.inputFlags.ICRNL_);
    impl_->termiosSettings.inputFlags.IUCLC.value(termiosState.inputFlags.IUCLC_);
    impl_->termiosSettings.inputFlags.IXON.value(termiosState.inputFlags.IXON_);
    impl_->termiosSettings.inputFlags.IXANY.value(termiosState.inputFlags.IXANY_);
    impl_->termiosSettings.inputFlags.IXOFF.value(termiosState.inputFlags.IXOFF_);
    impl_->termiosSettings.inputFlags.IMAXBEL.value(termiosState.inputFlags.IMAXBEL_);
    impl_->termiosSettings.inputFlags.IUTF8.value(termiosState.inputFlags.IUTF8_);

    impl_->termiosSettings.outputFlags.OPOST.value(termiosState.outputFlags.OPOST_);
    impl_->termiosSettings.outputFlags.OLCUC.value(termiosState.outputFlags.OLCUC_);
    impl_->termiosSettings.outputFlags.ONLCR.value(termiosState.outputFlags.ONLCR_);
    impl_->termiosSettings.outputFlags.OCRNL.value(termiosState.outputFlags.OCRNL_);
    impl_->termiosSettings.outputFlags.ONOCR.value(termiosState.outputFlags.ONOCR_);
    impl_->termiosSettings.outputFlags.ONLRET.value(termiosState.outputFlags.ONLRET_);
    impl_->termiosSettings.outputFlags.OFILL.value(termiosState.outputFlags.OFILL_);
    impl_->termiosSettings.outputFlags.OFDEL.value(termiosState.outputFlags.OFDEL_);
    impl_->termiosSettings.outputFlags.NLDLY.value(termiosState.outputFlags.NLDLY_);
    impl_->termiosSettings.outputFlags.CRDLY.value(termiosState.outputFlags.CRDLY_);
    impl_->termiosSettings.outputFlags.TABDLY.value(termiosState.outputFlags.TABDLY_);
    impl_->termiosSettings.outputFlags.BSDLY.value(termiosState.outputFlags.BSDLY_);
    impl_->termiosSettings.outputFlags.VTDLY.value(termiosState.outputFlags.VTDLY_);
    impl_->termiosSettings.outputFlags.FFDLY.value(termiosState.outputFlags.FFDLY_);

    impl_->termiosSettings.controlFlags.CBAUD.value(termiosState.controlFlags.CBAUD_);
    impl_->termiosSettings.controlFlags.CBAUDEX.value(termiosState.controlFlags.CBAUDEX_);
    impl_->termiosSettings.controlFlags.CSIZE.value(termiosState.controlFlags.CSIZE_);
    impl_->termiosSettings.controlFlags.CSTOPB.value(termiosState.controlFlags.CSTOPB_);
    impl_->termiosSettings.controlFlags.CREAD.value(termiosState.controlFlags.CREAD_);
    impl_->termiosSettings.controlFlags.PARENB.value(termiosState.controlFlags.PARENB_);
    impl_->termiosSettings.controlFlags.PARODD.value(termiosState.controlFlags.PARODD_);
    impl_->termiosSettings.controlFlags.HUPCL.value(termiosState.controlFlags.HUPCL_);
    impl_->termiosSettings.controlFlags.CLOCAL.value(termiosState.controlFlags.CLOCAL_);
    impl_->termiosSettings.controlFlags.LOBLK.value(termiosState.controlFlags.LOBLK_);
    impl_->termiosSettings.controlFlags.CIBAUD.value(termiosState.controlFlags.CIBAUD_);
    impl_->termiosSettings.controlFlags.CMSPAR.value(termiosState.controlFlags.CMSPAR_);
    impl_->termiosSettings.controlFlags.CRTSCTS.value(termiosState.controlFlags.CRTSCTS_);

    impl_->termiosSettings.localFlags.ISIG.value(termiosState.localFlags.ISIG_);
    impl_->termiosSettings.localFlags.ICANON.value(termiosState.localFlags.ICANON_);
    impl_->termiosSettings.localFlags.XCASE.value(termiosState.localFlags.XCASE_);
    impl_->termiosSettings.localFlags.ECHO.value(termiosState.localFlags.ECHO_);
    impl_->termiosSettings.localFlags.ECHOE.value(termiosState.localFlags.ECHOE_);
    impl_->termiosSettings.localFlags.ECHOK.value(termiosState.localFlags.ECHOK_);
    impl_->termiosSettings.localFlags.ECHONL.value(termiosState.localFlags.ECHONL_);
    impl_->termiosSettings.localFlags.ECHOCTL.value(termiosState.localFlags.ECHOCTL_);
    impl_->termiosSettings.localFlags.ECHOPRT.value(termiosState.localFlags.ECHOPRT_);
    impl_->termiosSettings.localFlags.ECHOKE.value(termiosState.localFlags.ECHOKE_);
    impl_->termiosSettings.localFlags.FLUSHO.value(termiosState.localFlags.FLUSHO_);
    impl_->termiosSettings.localFlags.NOFLSH.value(termiosState.localFlags.NOFLSH_);
    impl_->termiosSettings.localFlags.TOSTOP.value(termiosState.localFlags.TOSTOP_);
    impl_->termiosSettings.localFlags.PENDIN.value(termiosState.localFlags.PENDIN_);
    impl_->termiosSettings.localFlags.IEXTEN.value(termiosState.localFlags.IEXTEN_);

    impl_->termiosSettings.ccEngaged = termiosState.cc.has_value();
    if (impl_->termiosSettings.ccEngaged.value())
    {
        impl_->termiosSettings.cc.VDISCARD.value(termiosState.cc->VDISCARD_);
        impl_->termiosSettings.cc.VDSUSP.value(termiosState.cc->VDSUSP_);
        impl_->termiosSettings.cc.VEOF.value(termiosState.cc->VEOF_);
        impl_->termiosSettings.cc.VEOL.value(termiosState.cc->VEOL_);
        impl_->termiosSettings.cc.VEOL2.value(termiosState.cc->VEOL2_);
        impl_->termiosSettings.cc.VERASE.value(termiosState.cc->VERASE_);
        impl_->termiosSettings.cc.VINTR.value(termiosState.cc->VINTR_);
        impl_->termiosSettings.cc.VKILL.value(termiosState.cc->VKILL_);
        impl_->termiosSettings.cc.VLNEXT.value(termiosState.cc->VLNEXT_);
        impl_->termiosSettings.cc.VMIN.value(termiosState.cc->VMIN_);
        impl_->termiosSettings.cc.VQUIT.value(termiosState.cc->VQUIT_);
        impl_->termiosSettings.cc.VREPRINT.value(termiosState.cc->VREPRINT_);
        impl_->termiosSettings.cc.VSTART.value(termiosState.cc->VSTART_);
        impl_->termiosSettings.cc.VSTATUS.value(termiosState.cc->VSTATUS_);
        impl_->termiosSettings.cc.VSTOP.value(termiosState.cc->VSTOP_);
        impl_->termiosSettings.cc.VSUSP.value(termiosState.cc->VSUSP_);
        impl_->termiosSettings.cc.VSWTCH.value(termiosState.cc->VSWTCH_);
        impl_->termiosSettings.cc.VTIME.value(termiosState.cc->VTIME_);
        impl_->termiosSettings.cc.VWERASE.value(termiosState.cc->VWERASE_);
    }
    else
    {
        // All defaults:
        impl_->termiosSettings.cc.VDISCARD.value(Persistence::Termios::CC{}.VDISCARD_);
        impl_->termiosSettings.cc.VDSUSP.value(Persistence::Termios::CC{}.VDSUSP_);
        impl_->termiosSettings.cc.VEOF.value(Persistence::Termios::CC{}.VEOF_);
        impl_->termiosSettings.cc.VEOL.value(Persistence::Termios::CC{}.VEOL_);
        impl_->termiosSettings.cc.VEOL2.value(Persistence::Termios::CC{}.VEOL2_);
        impl_->termiosSettings.cc.VERASE.value(Persistence::Termios::CC{}.VERASE_);
        impl_->termiosSettings.cc.VINTR.value(Persistence::Termios::CC{}.VINTR_);
        impl_->termiosSettings.cc.VKILL.value(Persistence::Termios::CC{}.VKILL_);
        impl_->termiosSettings.cc.VLNEXT.value(Persistence::Termios::CC{}.VLNEXT_);
        impl_->termiosSettings.cc.VMIN.value(Persistence::Termios::CC{}.VMIN_);
        impl_->termiosSettings.cc.VQUIT.value(Persistence::Termios::CC{}.VQUIT_);
        impl_->termiosSettings.cc.VREPRINT.value(Persistence::Termios::CC{}.VREPRINT_);
        impl_->termiosSettings.cc.VSTART.value(Persistence::Termios::CC{}.VSTART_);
        impl_->termiosSettings.cc.VSTATUS.value(Persistence::Termios::CC{}.VSTATUS_);
        impl_->termiosSettings.cc.VSTOP.value(Persistence::Termios::CC{}.VSTOP_);
        impl_->termiosSettings.cc.VSUSP.value(Persistence::Termios::CC{}.VSUSP_);
        impl_->termiosSettings.cc.VSWTCH.value(Persistence::Termios::CC{}.VSWTCH_);
        impl_->termiosSettings.cc.VTIME.value(Persistence::Termios::CC{}.VTIME_);
        impl_->termiosSettings.cc.VWERASE.value(Persistence::Termios::CC{}.VWERASE_);
    }

    impl_->termiosSettings.iSpeed.value(termiosState.iSpeed);
    impl_->termiosSettings.oSpeed.value(termiosState.oSpeed);
}
void Settings::loadSshSettingsFromStateByKey(std::string const& key, Persistence::State const& state)
{
    if (!state.sshOptions.contains(key))
        return;

    const auto& sshOptions = state.sshOptions.at(key);

    impl_->sshOptions.sshDirectory.value(pathOptionalToStringOptional(sshOptions.sshDirectory));
    impl_->sshOptions.knownHostsFile.value(pathOptionalToStringOptional(sshOptions.knownHostsFile));
    impl_->sshOptions.tryAgentForAuthentication.value(sshOptions.tryAgentForAuthentication);
    impl_->sshOptions.usePublicKeyAutoAuth.value(sshOptions.usePublicKeyAutoAuth);
    impl_->sshOptions.logVerbosity.value(sshOptions.logVerbosity);
    impl_->sshOptions.keyExchangeAlgorithms.value(sshOptions.keyExchangeAlgorithms);
    impl_->sshOptions.compressionClientToServer.value(sshOptions.compressionClientToServer);
    impl_->sshOptions.compressionServerToClient.value(sshOptions.compressionServerToClient);
    impl_->sshOptions.compressionLevel.value(sshOptions.compressionLevel);
    impl_->sshOptions.strictHostKeyCheck.value(sshOptions.strictHostKeyCheck);
    impl_->sshOptions.proxyCommand.value(sshOptions.proxyCommand);
    impl_->sshOptions.gssapiServerIdentity.value(sshOptions.gssapiServerIdentity);
    impl_->sshOptions.gssapiClientIdentity.value(sshOptions.gssapiClientIdentity);
    impl_->sshOptions.gssapiDelegateCredentials.value(sshOptions.gssapiDelegateCredentials);
    impl_->sshOptions.noDelay.value(sshOptions.noDelay);
    impl_->sshOptions.bypassConfig.value(sshOptions.bypassConfig);
    impl_->sshOptions.identityAgent.value(sshOptions.identityAgent);
    impl_->sshOptions.connectTimeoutSeconds.value(sshOptions.connectTimeoutSeconds);
    impl_->sshOptions.connectTimeoutUSeconds.value(sshOptions.connectTimeoutUSeconds);
}
void Settings::loadSftpOptionsFromStateByKey(std::string const& key, Persistence::State const& state)
{
    if (!state.sftpOptions.contains(key))
        return;

    const auto& sftpOptions = state.sftpOptions.at(key);

    impl_->sftpOptions.downloadOptionsEngaged = sftpOptions.downloadOptions.has_value();
    if (impl_->sftpOptions.downloadOptionsEngaged.value())
    {
        impl_->sftpOptions.downloadOptions.tempFileSuffix.value(sftpOptions.downloadOptions->tempFileSuffix);
        impl_->sftpOptions.downloadOptions.mayOverwrite.value(sftpOptions.downloadOptions->mayOverwrite);
        impl_->sftpOptions.downloadOptions.tryContinue.value(sftpOptions.downloadOptions->tryContinue);
        impl_->sftpOptions.downloadOptions.inheritPermissions.value(sftpOptions.downloadOptions->inheritPermissions);
        impl_->sftpOptions.downloadOptions.customPermissions.value(
            filesystemPermsOptionalToUShortOptional(sftpOptions.downloadOptions->customPermissions)
        );
        impl_->sftpOptions.downloadOptions.reserveSpace.value(sftpOptions.downloadOptions->reserveSpace);
        impl_->sftpOptions.downloadOptions.doCleanup.value(sftpOptions.downloadOptions->doCleanup);
    }
    else
    {
        // All defaults:
        impl_->sftpOptions.downloadOptions.tempFileSuffix.value(std::nullopt);
        impl_->sftpOptions.downloadOptions.mayOverwrite.value(std::nullopt);
        impl_->sftpOptions.downloadOptions.tryContinue.value(std::nullopt);
        impl_->sftpOptions.downloadOptions.inheritPermissions.value(std::nullopt);
        impl_->sftpOptions.downloadOptions.customPermissions.value(std::nullopt);
        impl_->sftpOptions.downloadOptions.reserveSpace.value(std::nullopt);
        impl_->sftpOptions.downloadOptions.doCleanup.value(std::nullopt);
    }

    impl_->sftpOptions.uploadOptionsEngaged = sftpOptions.uploadOptions.has_value();
    if (!sftpOptions.uploadOptions.has_value())
    {
        // All defaults:
        impl_->sftpOptions.uploadOptions.tempFileSuffix.value(std::nullopt);
        impl_->sftpOptions.uploadOptions.mayOverwrite.value(std::nullopt);
        impl_->sftpOptions.uploadOptions.tryContinue.value(std::nullopt);
        impl_->sftpOptions.uploadOptions.inheritPermissions.value(std::nullopt);
        impl_->sftpOptions.uploadOptions.customPermissions.value(std::nullopt);
    }
    else
    {
        impl_->sftpOptions.uploadOptions.tempFileSuffix.value(sftpOptions.uploadOptions->tempFileSuffix);
        impl_->sftpOptions.uploadOptions.mayOverwrite.value(sftpOptions.uploadOptions->mayOverwrite);
        impl_->sftpOptions.uploadOptions.tryContinue.value(sftpOptions.uploadOptions->tryContinue);
        impl_->sftpOptions.uploadOptions.inheritPermissions.value(sftpOptions.uploadOptions->inheritPermissions);
        impl_->sftpOptions.uploadOptions.customPermissions.value(
            filesystemPermsOptionalToUShortOptional(sftpOptions.uploadOptions->customPermissions)
        );
    }
    impl_->sftpOptions.concurrency.value(sftpOptions.concurrency);
    impl_->sftpOptions.operationTimeoutSeconds.value(sftpOptions.operationTimeout.count());
}
void Settings::loadTerminalOptionsFromStateByKey(std::string const& key, Persistence::State const& state)
{
    if (!state.terminalOptions.contains(key))
        return;

    const auto& terminalOptions = state.terminalOptions.at(key);

    impl_->terminalOptions.fontFamily.value(terminalOptions.fontFamily);
    impl_->terminalOptions.fontSize.value(terminalOptions.fontSize);
    impl_->terminalOptions.lineHeight.value(terminalOptions.lineHeight);
    impl_->terminalOptions.cursorBlink.value(terminalOptions.cursorBlink);
    impl_->terminalOptions.renderer.value(terminalOptions.renderer);
    impl_->terminalOptions.letterSpacing.value(terminalOptions.letterSpacing);

    impl_->terminalOptions.themeEngaged = terminalOptions.theme.has_value();
    if (terminalOptions.theme.has_value())
    {
        const auto& theme = terminalOptions.theme.value();
        impl_->terminalOptions.theme.background.value(theme.background);
        impl_->terminalOptions.theme.black.value(theme.black);
        impl_->terminalOptions.theme.blue.value(theme.blue);
        impl_->terminalOptions.theme.brightBlack.value(theme.brightBlack);
        impl_->terminalOptions.theme.brightBlue.value(theme.brightBlue);
        impl_->terminalOptions.theme.brightCyan.value(theme.brightCyan);
        impl_->terminalOptions.theme.brightGreen.value(theme.brightGreen);
        impl_->terminalOptions.theme.brightMagenta.value(theme.brightMagenta);
        impl_->terminalOptions.theme.brightRed.value(theme.brightRed);
        impl_->terminalOptions.theme.brightWhite.value(theme.brightWhite);
        impl_->terminalOptions.theme.brightYellow.value(theme.brightYellow);
        impl_->terminalOptions.theme.cursor.value(theme.cursor);
        impl_->terminalOptions.theme.cursorAccent.value(theme.cursorAccent);
        impl_->terminalOptions.theme.cyan.value(theme.cyan);
        impl_->terminalOptions.theme.foreground.value(theme.foreground);
        impl_->terminalOptions.theme.green.value(theme.green);
        impl_->terminalOptions.theme.magenta.value(theme.magenta);
        impl_->terminalOptions.theme.red.value(theme.red);
        impl_->terminalOptions.theme.selectionBackground.value(theme.selectionBackground);
        impl_->terminalOptions.theme.selectionForeground.value(theme.selectionForeground);
        impl_->terminalOptions.theme.selectionInactiveBackground.value(theme.selectionInactiveBackground);
        impl_->terminalOptions.theme.white.value(theme.white);
        impl_->terminalOptions.theme.yellow.value(theme.yellow);
    }
    else
    {
        // All defaults:
        impl_->terminalOptions.theme.background.value(std::nullopt);
        impl_->terminalOptions.theme.black.value(std::nullopt);
        impl_->terminalOptions.theme.blue.value(std::nullopt);
        impl_->terminalOptions.theme.brightBlack.value(std::nullopt);
        impl_->terminalOptions.theme.brightBlue.value(std::nullopt);
        impl_->terminalOptions.theme.brightCyan.value(std::nullopt);
        impl_->terminalOptions.theme.brightGreen.value(std::nullopt);
        impl_->terminalOptions.theme.brightMagenta.value(std::nullopt);
        impl_->terminalOptions.theme.brightRed.value(std::nullopt);
        impl_->terminalOptions.theme.brightWhite.value(std::nullopt);
        impl_->terminalOptions.theme.brightYellow.value(std::nullopt);
        impl_->terminalOptions.theme.cursor.value(std::nullopt);
        impl_->terminalOptions.theme.cursorAccent.value(std::nullopt);
        impl_->terminalOptions.theme.cyan.value(std::nullopt);
        impl_->terminalOptions.theme.foreground.value(std::nullopt);
        impl_->terminalOptions.theme.green.value(std::nullopt);
        impl_->terminalOptions.theme.magenta.value(std::nullopt);
        impl_->terminalOptions.theme.red.value(std::nullopt);
        impl_->terminalOptions.theme.selectionBackground.value(std::nullopt);
        impl_->terminalOptions.theme.selectionForeground.value(std::nullopt);
        impl_->terminalOptions.theme.selectionInactiveBackground.value(std::nullopt);
        impl_->terminalOptions.theme.white.value(std::nullopt);
        impl_->terminalOptions.theme.yellow.value(std::nullopt);
    }
}
void Settings::loadQueueOptionsFromStateByKey(std::string const& key, Persistence::State const& state)
{
    if (!state.queueOptions.contains(key))
        return;

    const auto& queueOptions = state.queueOptions.at(key);

    impl_->queueOptions.autoRemoveCompletedOperations.value(queueOptions.autoRemoveCompletedOperations);
    impl_->queueOptions.startInPausedState.value(queueOptions.startInPausedState);
}

void Settings::applyTermiosSettingsToStateByKey(std::string const& key, Persistence::State& state)
{
    Persistence::Termios termiosEntry{
        .inputFlags =
            Persistence::Termios::InputFlags{
                .IGNBRK_ = impl_->termiosSettings.inputFlags.IGNBRK.value(),
                .BRKINT_ = impl_->termiosSettings.inputFlags.BRKINT.value(),
                .IGNPAR_ = impl_->termiosSettings.inputFlags.IGNPAR.value(),
                .PARMRK_ = impl_->termiosSettings.inputFlags.PARMRK.value(),
                .INPCK_ = impl_->termiosSettings.inputFlags.INPCK.value(),
                .ISTRIP_ = impl_->termiosSettings.inputFlags.ISTRIP.value(),
                .INLCR_ = impl_->termiosSettings.inputFlags.INLCR.value(),
                .IGNCR_ = impl_->termiosSettings.inputFlags.IGNCR.value(),
                .ICRNL_ = impl_->termiosSettings.inputFlags.ICRNL.value(),
                .IUCLC_ = impl_->termiosSettings.inputFlags.IUCLC.value(),
                .IXON_ = impl_->termiosSettings.inputFlags.IXON.value(),
                .IXANY_ = impl_->termiosSettings.inputFlags.IXANY.value(),
                .IXOFF_ = impl_->termiosSettings.inputFlags.IXOFF.value(),
                .IMAXBEL_ = impl_->termiosSettings.inputFlags.IMAXBEL.value(),
                .IUTF8_ = impl_->termiosSettings.inputFlags.IUTF8.value(),
            },
        .outputFlags =
            Persistence::Termios::OutputFlags{
                .OPOST_ = impl_->termiosSettings.outputFlags.OPOST.value(),
                .OLCUC_ = impl_->termiosSettings.outputFlags.OLCUC.value(),
                .ONLCR_ = impl_->termiosSettings.outputFlags.ONLCR.value(),
                .OCRNL_ = impl_->termiosSettings.outputFlags.OCRNL.value(),
                .ONOCR_ = impl_->termiosSettings.outputFlags.ONOCR.value(),
                .ONLRET_ = impl_->termiosSettings.outputFlags.ONLRET.value(),
                .OFILL_ = impl_->termiosSettings.outputFlags.OFILL.value(),
                .OFDEL_ = impl_->termiosSettings.outputFlags.OFDEL.value(),
                .NLDLY_ = impl_->termiosSettings.outputFlags.NLDLY.value(),
                .CRDLY_ = impl_->termiosSettings.outputFlags.CRDLY.value(),
                .TABDLY_ = impl_->termiosSettings.outputFlags.TABDLY.value(),
                .BSDLY_ = impl_->termiosSettings.outputFlags.BSDLY.value(),
                .VTDLY_ = impl_->termiosSettings.outputFlags.VTDLY.value(),
                .FFDLY_ = impl_->termiosSettings.outputFlags.FFDLY.value(),
            },
        .controlFlags =
            Persistence::Termios::ControlFlags{
                .CBAUD_ = impl_->termiosSettings.controlFlags.CBAUD.value(),
                .CBAUDEX_ = impl_->termiosSettings.controlFlags.CBAUDEX.value(),
                .CSIZE_ = impl_->termiosSettings.controlFlags.CSIZE.value(),
                .CSTOPB_ = impl_->termiosSettings.controlFlags.CSTOPB.value(),
                .CREAD_ = impl_->termiosSettings.controlFlags.CREAD.value(),
                .PARENB_ = impl_->termiosSettings.controlFlags.PARENB.value(),
                .PARODD_ = impl_->termiosSettings.controlFlags.PARODD.value(),
                .HUPCL_ = impl_->termiosSettings.controlFlags.HUPCL.value(),
                .CLOCAL_ = impl_->termiosSettings.controlFlags.CLOCAL.value(),
                .LOBLK_ = impl_->termiosSettings.controlFlags.LOBLK.value(),
                .CIBAUD_ = impl_->termiosSettings.controlFlags.CIBAUD.value(),
                .CMSPAR_ = impl_->termiosSettings.controlFlags.CMSPAR.value(),
                .CRTSCTS_ = impl_->termiosSettings.controlFlags.CRTSCTS.value(),
            },
        .localFlags =
            Persistence::Termios::LocalFlags{
                .ISIG_ = impl_->termiosSettings.localFlags.ISIG.value(),
                .ICANON_ = impl_->termiosSettings.localFlags.ICANON.value(),
                .XCASE_ = impl_->termiosSettings.localFlags.XCASE.value(),
                .ECHO_ = impl_->termiosSettings.localFlags.ECHO.value(),
                .ECHOE_ = impl_->termiosSettings.localFlags.ECHOE.value(),
                .ECHOK_ = impl_->termiosSettings.localFlags.ECHOK.value(),
                .ECHONL_ = impl_->termiosSettings.localFlags.ECHONL.value(),
                .ECHOCTL_ = impl_->termiosSettings.localFlags.ECHOCTL.value(),
                .ECHOPRT_ = impl_->termiosSettings.localFlags.ECHOPRT.value(),
                .ECHOKE_ = impl_->termiosSettings.localFlags.ECHOKE.value(),
                .FLUSHO_ = impl_->termiosSettings.localFlags.FLUSHO.value(),
                .NOFLSH_ = impl_->termiosSettings.localFlags.NOFLSH.value(),
                .TOSTOP_ = impl_->termiosSettings.localFlags.TOSTOP.value(),
                .PENDIN_ = impl_->termiosSettings.localFlags.PENDIN.value(),
                .IEXTEN_ = impl_->termiosSettings.localFlags.IEXTEN.value(),
            },
        .cc = impl_->termiosSettings.ccEngaged.value()
            ? std::optional<Persistence::Termios::CC>{Persistence::Termios::CC{
                  .VDISCARD_ = impl_->termiosSettings.cc.VDISCARD.value(),
                  .VDSUSP_ = impl_->termiosSettings.cc.VDSUSP.value(),
                  .VEOF_ = impl_->termiosSettings.cc.VEOF.value(),
                  .VEOL_ = impl_->termiosSettings.cc.VEOL.value(),
                  .VEOL2_ = impl_->termiosSettings.cc.VEOL2.value(),
                  .VERASE_ = impl_->termiosSettings.cc.VERASE.value(),
                  .VINTR_ = impl_->termiosSettings.cc.VINTR.value(),
                  .VKILL_ = impl_->termiosSettings.cc.VKILL.value(),
                  .VLNEXT_ = impl_->termiosSettings.cc.VLNEXT.value(),
                  .VMIN_ = impl_->termiosSettings.cc.VMIN.value(),
                  .VQUIT_ = impl_->termiosSettings.cc.VQUIT.value(),
                  .VREPRINT_ = impl_->termiosSettings.cc.VREPRINT.value(),
                  .VSTART_ = impl_->termiosSettings.cc.VSTART.value(),
                  .VSTATUS_ = impl_->termiosSettings.cc.VSTATUS.value(),
                  .VSTOP_ = impl_->termiosSettings.cc.VSTOP.value(),
                  .VSUSP_ = impl_->termiosSettings.cc.VSUSP.value(),
                  .VSWTCH_ = impl_->termiosSettings.cc.VSWTCH.value(),
                  .VTIME_ = impl_->termiosSettings.cc.VTIME.value(),
                  .VWERASE_ = impl_->termiosSettings.cc.VWERASE.value(),
              }}
            : std::nullopt,
        .iSpeed = impl_->termiosSettings.iSpeed.value(),
        .oSpeed = impl_->termiosSettings.oSpeed.value(),
    };
    state.termios[key] = std::move(termiosEntry);
}
void Settings::applySshSettingsToStateByKey(std::string const& key, Persistence::State& state)
{
    Persistence::SshOptions sshOptionsEntry{
        .sshDirectory = stringOptionalToPathOptional(impl_->sshOptions.sshDirectory.value()),
        .knownHostsFile = stringOptionalToPathOptional(impl_->sshOptions.knownHostsFile.value()),
        .tryAgentForAuthentication = impl_->sshOptions.tryAgentForAuthentication.value(),
        .usePublicKeyAutoAuth = impl_->sshOptions.usePublicKeyAutoAuth.value(),
        .logVerbosity = impl_->sshOptions.logVerbosity.value(),
        .keyExchangeAlgorithms = impl_->sshOptions.keyExchangeAlgorithms.value(),
        .compressionClientToServer = impl_->sshOptions.compressionClientToServer.value(),
        .compressionServerToClient = impl_->sshOptions.compressionServerToClient.value(),
        .compressionLevel = impl_->sshOptions.compressionLevel.value(),
        .strictHostKeyCheck = impl_->sshOptions.strictHostKeyCheck.value(),
        .proxyCommand = impl_->sshOptions.proxyCommand.value(),
        .gssapiServerIdentity = impl_->sshOptions.gssapiServerIdentity.value(),
        .gssapiClientIdentity = impl_->sshOptions.gssapiClientIdentity.value(),
        .gssapiDelegateCredentials = impl_->sshOptions.gssapiDelegateCredentials.value(),
        .noDelay = impl_->sshOptions.noDelay.value(),
        .bypassConfig = impl_->sshOptions.bypassConfig.value(),
        .identityAgent = impl_->sshOptions.identityAgent.value(),
        .connectTimeoutSeconds = impl_->sshOptions.connectTimeoutSeconds.value(),
        .connectTimeoutUSeconds = impl_->sshOptions.connectTimeoutUSeconds.value(),
    };
    state.sshOptions[key] = std::move(sshOptionsEntry);
}
void Settings::applySftpOptionsToStateByKey(std::string const& key, Persistence::State& state)
{
    Persistence::SftpOptions sftpOptionsEntry{};

    if (impl_->sftpOptions.downloadOptionsEngaged.value())
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-designator"
        sftpOptionsEntry.downloadOptions = Persistence::DownloadOptions{
            Persistence::CommonTransferOptions{
                .tempFileSuffix = impl_->sftpOptions.downloadOptions.tempFileSuffix.value(),
                .mayOverwrite = impl_->sftpOptions.downloadOptions.mayOverwrite.value(),
                .tryContinue = impl_->sftpOptions.downloadOptions.tryContinue.value(),
                .inheritPermissions = impl_->sftpOptions.downloadOptions.inheritPermissions.value(),
                .customPermissions = uShortOptionalToFilesystemPermsOptional(
                    impl_->sftpOptions.downloadOptions.customPermissions.value()
                ),
            },
            .reserveSpace = impl_->sftpOptions.downloadOptions.reserveSpace.value(),
            .doCleanup = impl_->sftpOptions.downloadOptions.doCleanup.value(),
        };
    }
#pragma clang diagnostic pop

    if (impl_->sftpOptions.uploadOptionsEngaged.value())
    {
        sftpOptionsEntry.uploadOptions = Persistence::UploadOptions{Persistence::CommonTransferOptions{
            .tempFileSuffix = impl_->sftpOptions.uploadOptions.tempFileSuffix.value(),
            .mayOverwrite = impl_->sftpOptions.uploadOptions.mayOverwrite.value(),
            .tryContinue = impl_->sftpOptions.uploadOptions.tryContinue.value(),
            .inheritPermissions = impl_->sftpOptions.uploadOptions.inheritPermissions.value(),
            .customPermissions =
                uShortOptionalToFilesystemPermsOptional(impl_->sftpOptions.uploadOptions.customPermissions.value()),
        }};
    }

    sftpOptionsEntry.concurrency = impl_->sftpOptions.concurrency.value();
    sftpOptionsEntry.operationTimeout = std::chrono::seconds{impl_->sftpOptions.operationTimeoutSeconds.value()};

    state.sftpOptions[key] = std::move(sftpOptionsEntry);
}
void Settings::applyTerminalOptionsToStateByKey(std::string const& key, Persistence::State& state)
{
    Persistence::TerminalOptions terminalOptionsEntry{
        .fontFamily = impl_->terminalOptions.fontFamily.value(),
        .fontSize = impl_->terminalOptions.fontSize.value(),
        .lineHeight = impl_->terminalOptions.lineHeight.value(),
        .cursorBlink = impl_->terminalOptions.cursorBlink.value(),
        .renderer = impl_->terminalOptions.renderer.value(),
        .letterSpacing = impl_->terminalOptions.letterSpacing.value(),
        .theme = impl_->terminalOptions.themeEngaged.value()
            ? std::optional<Persistence::TerminalTheme>{Persistence::TerminalTheme{
                  .background = impl_->terminalOptions.theme.background.value(),
                  .black = impl_->terminalOptions.theme.black.value(),
                  .blue = impl_->terminalOptions.theme.blue.value(),
                  .brightBlack = impl_->terminalOptions.theme.brightBlack.value(),
                  .brightBlue = impl_->terminalOptions.theme.brightBlue.value(),
                  .brightCyan = impl_->terminalOptions.theme.brightCyan.value(),
                  .brightGreen = impl_->terminalOptions.theme.brightGreen.value(),
                  .brightMagenta = impl_->terminalOptions.theme.brightMagenta.value(),
                  .brightRed = impl_->terminalOptions.theme.brightRed.value(),
                  .brightWhite = impl_->terminalOptions.theme.brightWhite.value(),
                  .brightYellow = impl_->terminalOptions.theme.brightYellow.value(),
                  .cursor = impl_->terminalOptions.theme.cursor.value(),
                  .cursorAccent = impl_->terminalOptions.theme.cursorAccent.value(),
                  .cyan = impl_->terminalOptions.theme.cyan.value(),
                  .foreground = impl_->terminalOptions.theme.foreground.value(),
                  .green = impl_->terminalOptions.theme.green.value(),
                  .magenta = impl_->terminalOptions.theme.magenta.value(),
                  .red = impl_->terminalOptions.theme.red.value(),
                  .selectionBackground = impl_->terminalOptions.theme.selectionBackground.value(),
                  .selectionForeground = impl_->terminalOptions.theme.selectionForeground.value(),
                  .selectionInactiveBackground = impl_->terminalOptions.theme.selectionInactiveBackground.value(),
                  .white = impl_->terminalOptions.theme.white.value(),
                  .yellow = impl_->terminalOptions.theme.yellow.value(),
              }}
            : std::nullopt,
    };
    state.terminalOptions[key] = std::move(terminalOptionsEntry);
}
void Settings::applyQueueOptionsToStateByKey(std::string const& key, Persistence::State& state)
{
    Persistence::QueueOptions queueOptionsEntry{
        .autoRemoveCompletedOperations = impl_->queueOptions.autoRemoveCompletedOperations.value(),
        .startInPausedState = impl_->queueOptions.startInPausedState.value(),
    };
    state.queueOptions[key] = std::move(queueOptionsEntry);
}

void Settings::onChange()
{
    impl_->saveInProgress = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this](bool, Persistence::State const&)
        {
            applySettingsToState(impl_->stateHolder->stateCache());

            impl_->stateHolder->save(
                [this](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .state = ConfirmDialog::State::Negative,
                            .headerText = language->get("settings", "errorSavingSettingsHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                    }

                    impl_->saveInProgress = false;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            );
        },
        language->get("settings", "errorLoadingSettings")
    );
}

Nui::ElementRenderer Settings::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    return div{
        class_ = "settings-page-background-blocker",
        style = observe(impl_->events->settingsOpen)
            .generate(
                [](bool isOpen) -> std::string
                {
                    return isOpen ? "display: flex;" : "display: none;";
                }
            ),
    }(impl_->newSessionDialog(),
        div{
            class_ = "settings-page",
        }(header(),
            div{
                class_ = "settings-page-content",
            }(side(),
                div{
                    class_ = "settings-page-content main",
                }(sections()))));
}

Nui::ElementRenderer Settings::sections()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        class_ = "settings-page-sections",
    }(
        observe(impl_->activeSection).generate(
            [this](Section activeSection) -> Nui::ElementRenderer {
                switch (activeSection) {
                    case Section::GeneralSettings:
                        return generalSettings();
                    case Section::GlobalInheritables:
                        return inheritableSettings();
                    case Section::Session:
                        return Nui::nil(); // TODO
                    default:
                        return Nui::nil(); // TODO
                }
                return Nui::nil();
            }
        )
    );
}

Nui::ElementRenderer Settings::header()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = "settings-page-header",
    }(
        iconPanel({
            .name = "action-settings",
            .color = "var(--sapBrandColor)",
            .withBorder = true
        }),
        div{class_ = "title"}(language->getObserved("settings", "title")),
        div{
            class_ = "save-indicator",
            style = observe(impl_->saveInProgress).generate([](bool inProgress) {
                return inProgress ? "visibility: visible;" : "visibility: hidden;";
            })
        }(
            ui5::busy_indicator{
                "size"_prop = "M",
            }(),
            span{}(language->getObserved("settings", "saving"))
        ),
        ui5::button{
            "design"_prop = "Transparent",
            "icon"_prop = "decline",
            "click"_event = [this]() {
                impl_->events->settingsOpen = false;
            },
        }()
    );
    // clang-format on
}

bool Settings::isActive(SectionSelectorOptions const& options)
{
    if (options.thisSection == Section::Session)
        return options.sessionId && *impl_->activeSession == options.sessionId.value_or("");
    else
    {
        return options.thisSection == *impl_->activeSection;
    }
}

void Settings::addNewSession()
{
    impl_->newSessionDialog.open(
        [](auto const& result)
        {
            Log::info("New session created: {} with icon {}", result.sessionName, result.iconName);
        }
    );
}

Nui::ElementRenderer Settings::sectionSelector(SectionSelectorOptions const& options)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = observe(impl_->activeSection, impl_->activeSession).generate(
            [this, options]()
            {
                return fmt::format("settings-page-section-selector {}", isActive(options) ? "active" : "");
            }
        ),
        onClick = [this, options]() {
            if (options.thisSection == Section::Add) {
                addNewSession();
                return;
            }

            if (options.thisSection == Section::Session && options.sessionId.has_value()) {
                impl_->activeSection = Section::Session;
                impl_->activeSession = options.sessionId;
            } else {
                impl_->activeSection = options.thisSection;
                impl_->activeSession = std::nullopt;
            }
        },
    }(
        observe(impl_->activeSection, impl_->activeSession),
        [this, options](){
            const auto active = isActive(options);

            return fragment(
                [icon = options.icon, active]() -> Nui::ElementRenderer {
                    if (icon.empty())
                        return Nui::nil();
                    return iconPanel({
                        .name = icon,
                        .color = active ? "var(--sapBrandColor)" : "#404040",
                        .withBorder = true
                    });
                }(),
                span{}(
                    observe(impl_->events->onLanguageChanged).generate(
                        [&options]() -> std::string {
                        if (options.sessionId.has_value())
                            return options.sessionId.value();

                        switch (options.thisSection) {
                            case Settings::Section::GeneralSettings:
                                return language->get("settings", "generalSettings");
                            case Settings::Section::GlobalInheritables:
                                return language->get("settings", "globalInheritables");
                            case Settings::Section::Session:
                                return language->get("settings", "unknownSession");
                            case Settings::Section::Add:
                                return language->get("settings", "addNew");
                        }
                    })
                )
            );
        }
    );
    // clang-format on
}

Nui::ElementRenderer Settings::side()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{class_ = "side"}(
        div{class_ = "configuration-text"}(
            ui5::icon{
                "name"_prop = "settings",
                "design"_prop = "Neutral"
            }(),
            span{}(language->getObserved("settings", "configuration"))
        ),
        sectionSelector({
            .thisSection = Settings::Section::GeneralSettings,
            .icon = "wrench",
        }),
        sectionSelector({
            .thisSection = Settings::Section::GlobalInheritables,
            .icon = "settings",
        }),
        div{style = "width: calc(100% - 20px); border-top: 1px solid gray; margin-bottom: 20px; margin-top: 10px"}(),
        div{class_ = "configuration-text"}(
            ui5::icon{
                "name"_prop = "it-system",
                "design"_prop = "Neutral"
            }(),
            span{}(language->getObserved("settings", "sessionsServers"))
        ),
        sectionSelector({
            .thisSection = Settings::Section::Add,
            .icon = "add",
        }),
        div{style = "display: flex; flex-direction: column;"}(
            impl_->sessionSelectors.map([this](long long, auto const& item) -> Nui::ElementRenderer {
                return sectionSelector({
                    .thisSection = Settings::Section::Session,
                    .sessionId = item.sessionId,
                    .icon = item.icon,
                });
            })
        )
    );
    // clang-format on
}

Nui::ElementRenderer Settings::generalSettings()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    auto loggingAndErrorReporting = fragment(
        impl_->generalSettings.logLevel(language->getObserved("settings", "logLevel"))
    );

    auto localization = fragment(
        impl_->generalSettings.localization.language(language->getObserved("language")),
        impl_->generalSettings.localization.dateTimeFormat(language->getObserved("settings", "general", "localization", "dateTimeFormatString"))
    );

    auto userInterface = fragment(
        impl_->generalSettings.userInterface.fileGridPathBarOnTop(
            language->getObserved("settings", "general", "userInterface", "fileGridPathBarOnTop")
        ),
        impl_->generalSettings.userInterface.fileGridExtensionIcons(
            language->getObserved("settings", "general", "userInterface", "fileGridExtensionIcons")
        )
    );

    auto localFilesystemOptions = fragment(
        impl_->generalSettings.localFilesystemOptions.preventDeletion(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventDeletion")
        ),
        impl_->generalSettings.localFilesystemOptions.preventRename(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventRename")
        ),
        impl_->generalSettings.localFilesystemOptions.preventCreateFile(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateFile")
        ),
        impl_->generalSettings.localFilesystemOptions.preventCreateDirectory(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateDirectory")
        ),
        impl_->generalSettings.localFilesystemOptions.homeOverride(
            language->getObserved("settings", "general", "localFilesystemOptions", "homeOverride")
        )
    );
    // clang-format on

    // clang-format off
    return fragment(
        group({
            .isCollapsed = impl_->collapsibleStates.localization,
            .content = std::move(localization),
            .headerTitle = language->getObserved("settings", "generalSettings")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.loggingAndErrorReporting,
            .content = std::move(loggingAndErrorReporting),
            .headerTitle = language->getObserved("settings", "loggingAndErrorReportingGroupHeader")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.userInterface,
            .content = std::move(userInterface),
            .headerTitle = language->getObserved("settings", "userInterfaceGroupHeader")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.localFilesystemOptions,
            .content = std::move(localFilesystemOptions),
            .headerTitle = language->getObserved("settings", "localFilesystemOptionsGroupHeader")
        })
    );
    // clang-format on
}

Nui::ElementRenderer Settings::inheritableSettings()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    auto sshOptions = fragment(
        impl_->sshOptions.sshDirectory(language->getObserved("settings", "sshOptions", "sshDirectory")),
        impl_->sshOptions.knownHostsFile(language->getObserved("settings", "sshOptions", "knownHostsFile")),
        impl_->sshOptions.tryAgentForAuthentication(language->getObserved("settings", "sshOptions", "tryAgentForAuthentication")),
        impl_->sshOptions.usePublicKeyAutoAuth(language->getObserved("settings", "sshOptions", "usePublicKeyAutoAuth")),
        impl_->sshOptions.logVerbosity(language->getObserved("settings", "sshOptions", "logVerbosity")),
        impl_->sshOptions.keyExchangeAlgorithms(language->getObserved("settings", "sshOptions", "keyExchangeAlgorithms")),
        impl_->sshOptions.compressionClientToServer(language->getObserved("settings", "sshOptions", "compressionClientToServer")),
        impl_->sshOptions.compressionServerToClient(language->getObserved("settings", "sshOptions", "compressionServerToClient")),
        impl_->sshOptions.compressionLevel(language->getObserved("settings", "sshOptions", "compressionLevel")),
        impl_->sshOptions.strictHostKeyCheck(language->getObserved("settings", "sshOptions", "strictHostKeyCheck")),
        impl_->sshOptions.proxyCommand(language->getObserved("settings", "sshOptions", "proxyCommand")),
        impl_->sshOptions.gssapiServerIdentity(language->getObserved("settings", "sshOptions", "gssapiServerIdentity")),
        impl_->sshOptions.gssapiClientIdentity(language->getObserved("settings", "sshOptions", "gssapiClientIdentity")),
        impl_->sshOptions.gssapiDelegateCredentials(language->getObserved("settings", "sshOptions", "gssapiDelegateCredentials")),
        impl_->sshOptions.noDelay(language->getObserved("settings", "sshOptions", "noDelay")),
        impl_->sshOptions.bypassConfig(language->getObserved("settings", "sshOptions", "bypassConfig")),
        impl_->sshOptions.identityAgent(language->getObserved("settings", "sshOptions", "identityAgent")),
        impl_->sshOptions.connectTimeoutSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutSeconds")),
        impl_->sshOptions.connectTimeoutUSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutUSeconds"))
    );
    // clang-format on

    // clang-format off
    auto sftpOptions = fragment(
        subgroup({
            .engagedStatus = &impl_->sftpOptions.downloadOptionsEngaged,
            .groupTitle = language->getObserved("settings", "sftpOptions", "downloadOptionsSubgroupTitle")
        }, fragment(
            impl_->sftpOptions.downloadOptions.tempFileSuffix(language->getObserved("settings", "sftpOptions", "downloadOptions", "tempFileSuffix")),
            impl_->sftpOptions.downloadOptions.mayOverwrite(language->getObserved("settings", "sftpOptions", "downloadOptions", "mayOverwrite")),
            impl_->sftpOptions.downloadOptions.tryContinue(language->getObserved("settings", "sftpOptions", "downloadOptions", "tryContinue")),
            impl_->sftpOptions.downloadOptions.inheritPermissions(language->getObserved("settings", "sftpOptions", "downloadOptions", "inheritPermissions")),
            impl_->sftpOptions.downloadOptions.customPermissions(language->getObserved("settings", "sftpOptions", "downloadOptions", "customPermissions")),
            impl_->sftpOptions.downloadOptions.reserveSpace(language->getObserved("settings", "sftpOptions", "downloadOptions", "reserveSpace")),
            impl_->sftpOptions.downloadOptions.doCleanup(language->getObserved("settings", "sftpOptions", "downloadOptions", "doCleanup"))
        )),
        subgroup({
            .engagedStatus = &impl_->sftpOptions.uploadOptionsEngaged,
            .groupTitle = language->getObserved("settings", "sftpOptions", "uploadOptionsSubgroupTitle")
        }, fragment(
            impl_->sftpOptions.uploadOptions.tempFileSuffix(language->getObserved("settings", "sftpOptions", "uploadOptions", "tempFileSuffix")),
            impl_->sftpOptions.uploadOptions.mayOverwrite(language->getObserved("settings", "sftpOptions", "uploadOptions", "mayOverwrite")),
            impl_->sftpOptions.uploadOptions.tryContinue(language->getObserved("settings", "sftpOptions", "uploadOptions", "tryContinue")),
            impl_->sftpOptions.uploadOptions.inheritPermissions(language->getObserved("settings", "sftpOptions", "uploadOptions", "inheritPermissions")),
            impl_->sftpOptions.uploadOptions.customPermissions(language->getObserved("settings", "sftpOptions", "uploadOptions", "customPermissions"))
        )),
        impl_->sftpOptions.concurrency(language->getObserved("settings", "sftpOptions", "concurrency")),
        impl_->sftpOptions.operationTimeoutSeconds(language->getObserved("settings", "sftpOptions", "operationTimeoutSeconds"))
    );
    // clang-format on

    // clang-format off
    auto terminalOptions = fragment(
        impl_->terminalOptions.fontFamily(language->getObserved("settings", "terminalOptions", "fontFamily")),
        impl_->terminalOptions.fontSize(language->getObserved("settings", "terminalOptions", "fontSize")),
        impl_->terminalOptions.lineHeight(language->getObserved("settings", "terminalOptions", "lineHeight")),
        impl_->terminalOptions.cursorBlink(language->getObserved("settings", "terminalOptions", "cursorBlink")),
        impl_->terminalOptions.renderer(language->getObserved("settings", "terminalOptions", "renderer")),
        impl_->terminalOptions.letterSpacing(language->getObserved("settings", "terminalOptions", "letterSpacing")),
        subgroup({
            .engagedStatus = &impl_->terminalOptions.themeEngaged,
            .groupTitle = language->getObserved("settings", "terminalOptions", "themeSubgroupTitle")
        }, fragment(
            impl_->terminalOptions.theme.background(language->getObserved("settings", "terminalOptions", "theme", "background")),
            impl_->terminalOptions.theme.black(language->getObserved("settings", "terminalOptions", "theme", "black")),
            impl_->terminalOptions.theme.blue(language->getObserved("settings", "terminalOptions", "theme", "blue")),
            impl_->terminalOptions.theme.brightBlack(language->getObserved("settings", "terminalOptions", "theme", "brightBlack")),
            impl_->terminalOptions.theme.brightBlue(language->getObserved("settings", "terminalOptions", "theme", "brightBlue")),
            impl_->terminalOptions.theme.brightCyan(language->getObserved("settings", "terminalOptions", "theme", "brightCyan")),
            impl_->terminalOptions.theme.brightGreen(language->getObserved("settings", "terminalOptions", "theme", "brightGreen")),
            impl_->terminalOptions.theme.brightMagenta(language->getObserved("settings", "terminalOptions", "theme", "brightMagenta")),
            impl_->terminalOptions.theme.brightRed(language->getObserved("settings", "terminalOptions", "theme", "brightRed")),
            impl_->terminalOptions.theme.brightWhite(language->getObserved("settings", "terminalOptions", "theme", "brightWhite")),
            impl_->terminalOptions.theme.brightYellow(language->getObserved("settings", "terminalOptions", "theme", "brightYellow")),
            impl_->terminalOptions.theme.cursor(language->getObserved("settings", "terminalOptions", "theme", "cursor")),
            impl_->terminalOptions.theme.cursorAccent(language->getObserved("settings", "terminalOptions", "theme", "cursorAccent")),
            impl_->terminalOptions.theme.cyan(language->getObserved("settings", "terminalOptions", "theme", "cyan")),
            impl_->terminalOptions.theme.foreground(language->getObserved("settings", "terminalOptions", "theme", "foreground")),
            impl_->terminalOptions.theme.green(language->getObserved("settings", "terminalOptions", "theme", "green")),
            impl_->terminalOptions.theme.magenta(language->getObserved("settings", "terminalOptions", "theme", "magenta")),
            impl_->terminalOptions.theme.red(language->getObserved("settings", "terminalOptions", "theme", "red")),
            impl_->terminalOptions.theme.selectionBackground(language->getObserved("settings", "terminalOptions", "theme", "selectionBackground")),
            impl_->terminalOptions.theme.selectionForeground(language->getObserved("settings", "terminalOptions", "theme", "selectionForeground")),
            impl_->terminalOptions.theme.selectionInactiveBackground(language->getObserved("settings", "terminalOptions", "theme", "selectionInactiveBackground")),
            impl_->terminalOptions.theme.white(language->getObserved("settings", "terminalOptions", "theme", "white")),
            impl_->terminalOptions.theme.yellow(language->getObserved("settings", "terminalOptions", "theme", "yellow"))
        ))
    );
    // clang-format on

    // clang-format off
    auto queueOptions = fragment(
        impl_->queueOptions.autoRemoveCompletedOperations(language->getObserved("settings", "queueOptions", "autoRemoveCompletedOperations")),
        impl_->queueOptions.startInPausedState(language->getObserved("settings", "queueOptions", "startInPausedState"))
    );
    // clang-format on

    // clang-format off
    auto termios = fragment(
        span{}(language->getObserved("settings", "termios", "inputFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.inputFlags.IGNBRK("IGNBRK"),
            impl_->termiosSettings.inputFlags.BRKINT("BRKINT"),
            impl_->termiosSettings.inputFlags.IGNPAR("IGNPAR"),
            impl_->termiosSettings.inputFlags.PARMRK("PARMRK"),
            impl_->termiosSettings.inputFlags.INPCK("INPCK"),
            impl_->termiosSettings.inputFlags.ISTRIP("ISTRIP"),
            impl_->termiosSettings.inputFlags.INLCR("INLCR"),
            impl_->termiosSettings.inputFlags.IGNCR("IGNCR"),
            impl_->termiosSettings.inputFlags.ICRNL("ICRNL"),
            impl_->termiosSettings.inputFlags.IUCLC("IUCLC"),
            impl_->termiosSettings.inputFlags.IXON("IXON"),
            impl_->termiosSettings.inputFlags.IXANY("IXANY"),
            impl_->termiosSettings.inputFlags.IXOFF("IXOFF"),
            impl_->termiosSettings.inputFlags.IMAXBEL("IMAXBEL"),
            impl_->termiosSettings.inputFlags.IUTF8("IUTF8")
        )),
        span{}(language->getObserved("settings", "termios", "outputFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.outputFlags.OPOST("OPOST"),
            impl_->termiosSettings.outputFlags.OLCUC("OLCUC"),
            impl_->termiosSettings.outputFlags.ONLCR("ONLCR"),
            impl_->termiosSettings.outputFlags.OCRNL("OCRNL"),
            impl_->termiosSettings.outputFlags.ONOCR("ONOCR"),
            impl_->termiosSettings.outputFlags.ONLRET("ONLRET"),
            impl_->termiosSettings.outputFlags.OFILL("OFILL"),
            impl_->termiosSettings.outputFlags.OFDEL("OFDEL"),
            impl_->termiosSettings.outputFlags.NLDLY("NLDLY"),
            impl_->termiosSettings.outputFlags.CRDLY("CRDLY"),
            impl_->termiosSettings.outputFlags.TABDLY("TABDLY"),
            impl_->termiosSettings.outputFlags.BSDLY("BSDLY"),
            impl_->termiosSettings.outputFlags.VTDLY("VTDLY"),
            impl_->termiosSettings.outputFlags.FFDLY("FFDLY")
        )),
        span{}(language->getObserved("settings", "termios", "controlFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.controlFlags.CBAUD("CBAUD"),
            impl_->termiosSettings.controlFlags.CBAUDEX("CBAUDEX"),
            impl_->termiosSettings.controlFlags.CSIZE("CSIZE"),
            impl_->termiosSettings.controlFlags.CSTOPB("CSTOPB"),
            impl_->termiosSettings.controlFlags.CREAD("CREAD"),
            impl_->termiosSettings.controlFlags.PARENB("PARENB"),
            impl_->termiosSettings.controlFlags.PARODD("PARODD"),
            impl_->termiosSettings.controlFlags.HUPCL("HUPCL"),
            impl_->termiosSettings.controlFlags.CLOCAL("CLOCAL"),
            impl_->termiosSettings.controlFlags.LOBLK("LOBLK"),
            impl_->termiosSettings.controlFlags.CIBAUD("CIBAUD"),
            impl_->termiosSettings.controlFlags.CMSPAR("CMSPAR"),
            impl_->termiosSettings.controlFlags.CRTSCTS("CRTSCTS")
        )),
        span{}(language->getObserved("settings", "termios", "localFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.localFlags.ISIG("ISIG"),
            impl_->termiosSettings.localFlags.ICANON("ICANON"),
            impl_->termiosSettings.localFlags.XCASE("XCASE"),
            impl_->termiosSettings.localFlags.ECHO("ECHO"),
            impl_->termiosSettings.localFlags.ECHOE("ECHOE"),
            impl_->termiosSettings.localFlags.ECHOK("ECHOK"),
            impl_->termiosSettings.localFlags.ECHONL("ECHONL"),
            impl_->termiosSettings.localFlags.ECHOPRT("ECHOPRT"),
            impl_->termiosSettings.localFlags.ECHOKE("ECHOKE"),
            impl_->termiosSettings.localFlags.FLUSHO("FLUSHO"),
            impl_->termiosSettings.localFlags.NOFLSH("NOFLSH"),
            impl_->termiosSettings.localFlags.TOSTOP("TOSTOP"),
            impl_->termiosSettings.localFlags.PENDIN("PENDIN"),
            impl_->termiosSettings.localFlags.IEXTEN("IEXTEN")
        )),
        span{}(language->getObserved("settings", "termios", "ccSettingsSubgroupTitle")),
        subgroup({
            .engagedStatus = &impl_->termiosSettings.ccEngaged,
            .groupTitle = language->getObserved("settings", "ccSettingsSubgroupTitle")
        }, fragment(
            impl_->termiosSettings.cc.VDISCARD("VDISCARD"),
            impl_->termiosSettings.cc.VDSUSP("VDSUSP"),
            impl_->termiosSettings.cc.VEOF("VEOF"),
            impl_->termiosSettings.cc.VEOL("VEOL"),
            impl_->termiosSettings.cc.VEOL2("VEOL2"),
            impl_->termiosSettings.cc.VERASE("VERASE"),
            impl_->termiosSettings.cc.VINTR("VINTR"),
            impl_->termiosSettings.cc.VKILL("VKILL"),
            impl_->termiosSettings.cc.VLNEXT("VLNEXT"),
            impl_->termiosSettings.cc.VMIN("VMIN"),
            impl_->termiosSettings.cc.VQUIT("VQUIT"),
            impl_->termiosSettings.cc.VREPRINT("VREPRINT"),
            impl_->termiosSettings.cc.VSTART("VSTART"),
            impl_->termiosSettings.cc.VSTATUS("VSTATUS"),
            impl_->termiosSettings.cc.VSTOP("VSTOP"),
            impl_->termiosSettings.cc.VSUSP("VSUSP"),
            impl_->termiosSettings.cc.VSWTCH("VSWTCH"),
            impl_->termiosSettings.cc.VTIME("VTIME"),
            impl_->termiosSettings.cc.VWERASE("VWERASE")
        )),
        impl_->termiosSettings.iSpeed(language->getObserved("settings", "termios", "iSpeedHelpText")),
        impl_->termiosSettings.oSpeed(language->getObserved("settings", "termios", "oSpeedHelpText"))
    );
    // clang-format on

    // clang-format off
    return fragment(
        ui5::message_strip{
            "design"_prop = "Information",
            "hideCloseButton"_prop = true,
        }(
            language->getObserved("settings", "inheritableSettingsInfoMessage")
        ),
        group({
            .isCollapsed = impl_->collapsibleStates.sshOptions,
            .content = std::move(sshOptions),
            .headerTitle = language->getObserved("settings", "sshOptionsGroupName"),
            .currentGroupKey = &impl_->sshOptions.groupKey,
            .groupKeys = &impl_->sshOptions.groupKeys,
            .isInheritableGroup = true,
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.sftpOptions,
            .content = std::move(sftpOptions),
            .headerTitle = language->getObserved("settings", "sftpOptionsGroupName"),
            .currentGroupKey = &impl_->sftpOptions.groupKey,
            .groupKeys = &impl_->sftpOptions.groupKeys,
            .isInheritableGroup = true,
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.terminalOptions,
            .content = std::move(terminalOptions),
            .headerTitle = language->getObserved("settings", "terminalOptionsGroupName"),
            .currentGroupKey = &impl_->terminalOptions.groupKey,
            .groupKeys = &impl_->terminalOptions.groupKeys,
            .isInheritableGroup = true,
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.queueOptions,
            .content = std::move(queueOptions),
            .headerTitle = language->getObserved("settings", "queueOptionsGroupName"),
            .currentGroupKey = &impl_->queueOptions.groupKey,
            .groupKeys = &impl_->queueOptions.groupKeys,
            .isInheritableGroup = true,
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.termios,
            .content = std::move(termios),
            .headerTitle = language->getObserved("settings", "termiosGroupName"),
            .currentGroupKey = &impl_->termiosSettings.groupKey,
            .groupKeys = &impl_->termiosSettings.groupKeys,
            .isInheritableGroup = true,
        })
    );
    // clang-format on
}

Nui::ElementRenderer Settings::subgroup(SubgroupParameters&& params, Nui::ElementRenderer content)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = "settings-subgroup",
    }(
        div{
            class_ = "settings-subgroup-header",
            style = params.engagedStatus ? "border: 1px solid var(--sapContent_ForegroundBorderColor); background-color: var(--darkerBackground)" : "",
            onClick = [engagedStatus = params.engagedStatus]() {
                if (engagedStatus)
                    *engagedStatus = !*engagedStatus;
            }
        }(
            // switch to enable/disable entire subgroup:
            [this, engagedStatus = params.engagedStatus, title = std::move(params.groupTitle)]() mutable -> Nui::ElementRenderer {
                if (!engagedStatus || !title)
                    return Nui::nil();

                return fragment(
                    ui5::label{
                        "design"_prop = "Bold",
                    }(std::move(title).value()),
                    ui5::switch_{
                        "checked"_prop = engagedStatus->value(), // initial not observed
                        "change"_event = [this, engagedStatus](Nui::val event) {
                            *engagedStatus = event["target"]["checked"].as<bool>();
                            onChange();
                        },
                    }()
                );
            }()
        ),
        div{
            class_ = "settings-subgroup-content",
            style = params.engagedStatus ? "margin-top: 20px; padding-top: 32px;" : ""
        }(
            std::move(content)
        )
    );
    // clang-format on
}

Nui::ElementRenderer Settings::group(GroupParameters&& params)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    auto groupKeyContainer = [&params, this]() -> Nui::ElementRenderer
    {
        if (!params.currentGroupKey)
            return Nui::nil();

        auto addGroupButton = [&params, this]() -> Nui::ElementRenderer
        {
            if (!params.isInheritableGroup)
                return Nui::nil();

            return ui5::button{
                "design"_prop = "Primary",
                "icon"_prop = "add",
                "click"_event = [this, currentGroupKey = params.currentGroupKey, groupKeys = params.groupKeys]()
                {
                    impl_->inputDialog->open({
                        .whatFor = language->get("settings", "groupKey"),
                        .prompt = language->get("settings", "enterGroupKeyPlaceholder"),
                        .headerText = language->get("settings", "groupKey"),
                        .isPassword = false,
                        .onConfirm = [this, currentGroupKey, groupKeys](std::optional<std::string> const& result)
                        {
                            if (result && !result->empty())
                            {
                                *currentGroupKey = *result;
                                if (std::find((*groupKeys)->begin(), (*groupKeys)->end(), *result) ==
                                    (*groupKeys)->end())
                                {
                                    (*groupKeys)->push_back(*result);
                                    groupKeys->modify();
                                    Nui::globalEventContext.executeActiveEventsImmediately();
                                }
                                else
                                {
                                    // Key already exists, do nothing or show a message if needed
                                    impl_->confirmDialog->open({
                                        .state = ConfirmDialog::State::Critical,
                                        .headerText = language->get("settings", "groupKeyExistsHeader"),
                                        .text = language->get("settings", "groupKeyExistsText"),
                                        .buttons = ConfirmDialog::Button::Ok,
                                    });
                                }
                            }
                        },
                    });
                },
            }();
        };

        auto removeGroupButton = [&params, this]() -> Nui::ElementRenderer
        {
            if (!params.isInheritableGroup)
                return Nui::nil();

            return ui5::button{
                "design"_prop = "Negative",
                "icon"_prop = "delete",
                "click"_event = [this, currentGroupKey = params.currentGroupKey, groupKeys = params.groupKeys]()
                {
                    impl_->confirmDialog->open({
                        .state = ConfirmDialog::State::Critical,
                        .headerText = language->get("settings", "confirmDeleteGroupKeyHeader"),
                        .text = language->get("settings", "confirmDeleteGroupKeyText"),
                        .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
                        .onClose = [currentGroupKey, groupKeys](ConfirmDialog::Button btn)
                        {
                            if (btn == ConfirmDialog::Button::Ok)
                            {
                                groupKeys->erase(
                                    std::remove((*groupKeys)->begin(), (*groupKeys)->end(), currentGroupKey->value()),
                                    (*groupKeys)->end()
                                );
                                groupKeys->modify();
                                if (!(*groupKeys)->empty())
                                {
                                    *currentGroupKey = ((*groupKeys)->front());
                                }
                                else
                                {
                                    *currentGroupKey = "default"s;
                                    (*groupKeys)->push_back("default"s);
                                    groupKeys->modify();
                                }
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        },
                    });
                },
            }();
        };

        return div{class_ = "settings-group-key-container"}(
            ui5::label{
                "design"_prop = "Bold",
            }(language->getObserved("settings", "groupKey")),
            ui5::select{
                "change"_event =
                    [this, currentGroupKey = params.currentGroupKey](Nui::val event)
                {
                    *currentGroupKey = event["detail"]["selectedOption"]["leKey"].as<std::string>();
                    loadTermiosSettingsFromStateByKey(currentGroupKey->value(), impl_->stateHolder->stateCache());
                },
                "value"_prop = *params.currentGroupKey,
            }(params.groupKeys->map(
                [](long long, std::string const& inheritKey) -> Nui::ElementRenderer
                {
                    return ui5::option{
                        "leKey"_prop = inheritKey,
                    }(inheritKey);
                }
            )),
            addGroupButton(),
            removeGroupButton()
        );
    };

    // clang-format off
    return div{
        class_ = "settings-group",
    }(
        div{
            class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
                return classes("settings-group-header", isCollapsed ? "collapsed" : "uncollapsed");
            }),
            onClick = [&isCollapsed = params.isCollapsed](){
                isCollapsed = !*isCollapsed;
            }
        }(
            // collapse indicator:
            span{class_ = "settings-group-header-collapse-indicator"}(
                ui5::icon{
                    "name"_prop = observe(params.isCollapsed).generate([](bool isCollapsed) {
                        return isCollapsed ? "navigation-right-arrow" : "navigation-down-arrow";
                    }),
                    "design"_prop = "Neutral",
                    style = "color: var(--sapTextColor)"
                }()
            ),
            span{class_ = "settings-group-header-title"}(std::move(params.headerTitle)),
            [&params]() -> Nui::ElementRenderer {
                if (params.isEnabled) {
                    return ui5::switch_{
                        "design"_prop = "Emphasized",
                        "state"_prop = *params.isEnabled,
                        "change"_event = [&params](Nui::val event) {
                            *params.isEnabled = event["state"].as<bool>();
                        },
                    }();
                }
                return Nui::nil();
            }()
        ),
        div{
            class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
                return classes("settings-group-content", isCollapsed ? "collapsed" : "uncollapsed");
            }),
            style = Nui::Attributes::Style{
                "padding-top"_style = params.currentGroupKey ? "0px" : "8px"
            },
        }(
            groupKeyContainer(),
            std::move(params.content)
        )
    );
    // clang-format on
}