#include <frontend/settings.hpp>

#include <frontend/components/icon_panel.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/classes.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/map_setting.hpp>
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

struct GeneralSettings
{
    struct CollapsibleStates
    {
        Nui::Observed<bool> localization{false};
        Nui::Observed<bool> loggingAndErrorReporting{false};
        Nui::Observed<bool> userInterface{false};
        Nui::Observed<bool> localFilesystemOptions{false};
    } collapsibleStates;

    ComboSetting<Log::Level, std::string> logLevel;

    struct Localization
    {
        ComboSetting<std::string, std::string> language;
        TextSetting<> dateTimeFormat;
    } localization;

    struct UserInterface
    {
        BoolSetting<> fileGridPathBarOnTop;
        MapSetting<> fileGridExtensionIcons;
    } userInterface;

    struct LocalFilesystemOptions
    {
        BoolSetting<> preventDeletion;
        BoolSetting<> preventRename;
        BoolSetting<> preventCreateFile;
        BoolSetting<> preventCreateDirectory;
        TextSetting<true> homeOverride;
    } localFilesystemOptions;

    GeneralSettings(std::invocable auto const& onChange, FrontendEvents* events)
        : logLevel{
              {
                  Log::Level::Trace,
                  Log::Level::Debug,
                  Log::Level::Info,
                  Log::Level::Warning,
                  Log::Level::Error,
                  Log::Level::Critical,
                  Log::Level::Off,
              },
              language->getObserved("settings", "general", "loggingAndErrorReporting", "logLevelHelpText"),
              onChange,
              [this, onChange]()
              {
                  logLevel.value(Persistence::State{}.logLevel);
                  onChange();
              },
              [](Log::Level const& level)
              {
                  return Utility::enumToString<Log::Level>(level);
              },
              [](Log::Level const& level) -> std::optional<std::string>
              {
                  switch (level)
                  {
                      case Log::Level::Trace:
                          return "activity-items";
                      case Log::Level::Debug:
                          return "zoom-in";
                      case Log::Level::Info:
                          return "information";
                      case Log::Level::Warning:
                          return "alert";
                      case Log::Level::Error:
                          return "error";
                      case Log::Level::Critical:
                          return "incident";
                      case Log::Level::Off:
                          return "hide";
                      default:
                          return std::nullopt;
                  }
              }
          }
        , localization{
              .language = {
                  {"en_US", "de_DE"},
                  language->getObserved("settings", "general", "localization", "languageHelpText"),
                  [onChange, this, events]()
                  {
                      onChange();
                      events->onLanguageChanged = localization.language.value();
                      events->onLanguageChanged.modifyNow();
                  },
                  [this, events, onChange]()
                  {
                      localization.language.value(Persistence::State{}.localizationOptions.languageCode);
                      events->onLanguageChanged = localization.language.value();
                      events->onLanguageChanged.modifyNow();
                      onChange();
                  },
                  [](std::string const& code) -> std::string
                  {
                      if (code == "en_US")
                          return "English (US)";
                      else if (code == "de_DE")
                          return "Deutsch";
                      return code;
                  },
              },
              .dateTimeFormat = TextSetting<>{
                  language->getObserved("settings", "general", "localization", "dateTimeFormatHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      localization.dateTimeFormat.value(Persistence::State{}.localizationOptions.dateTimeFormatString);
                      onChange();
                  },
              },
          }
          ,userInterface{
                .fileGridPathBarOnTop = BoolSetting<>{
                    language->getObserved("settings", "general", "userInterface", "fileGridPathBarOnTopHelpText"),
                    onChange,
                    [this, onChange]()
                    {
                        userInterface.fileGridPathBarOnTop.value(
                            Persistence::UiOptions{}.fileGridPathBarOnTop
                        );
                        onChange();
                    },
                },
                .fileGridExtensionIcons = MapSetting<>{
                    language->getObserved("settings", "general", "userInterface", "fileGridExtensionIconsHelpText"),
                    onChange,
                    [this, onChange]()
                    {
                        userInterface.fileGridExtensionIcons.value(
                            Persistence::UiOptions{}.fileGridExtensionIcons
                        );
                        onChange();
                    }
                },
          }, localFilesystemOptions{
              .preventDeletion = {
                  language->getObserved("settings", "general", "localFilesystemOptions", "preventDeletionHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      localFilesystemOptions.preventDeletion.value(
                          Persistence::LocalFilesystemOptions{}.preventDeletion
                      );
                      onChange();
                  },
              },
              .preventRename = {
                  language->getObserved("settings", "general", "localFilesystemOptions", "preventRenameHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      localFilesystemOptions.preventRename.value(
                          Persistence::LocalFilesystemOptions{}.preventRename
                      );
                      onChange();
                  },
              },
              .preventCreateFile = {
                  language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateFileHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      localFilesystemOptions.preventCreateFile.value(
                          Persistence::LocalFilesystemOptions{}.preventCreateFile
                      );
                      onChange();
                  },
              },
              .preventCreateDirectory = {
                  language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateDirectoryHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      localFilesystemOptions.preventCreateDirectory.value(
                          Persistence::LocalFilesystemOptions{}.preventCreateDirectory
                      );
                      onChange();
                  },
              },
              .homeOverride = TextSetting<true>{
                  language->getObserved("settings", "general", "localFilesystemOptions", "homeOverrideHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      localFilesystemOptions.homeOverride.value(
                          Persistence::LocalFilesystemOptions{}.homeOverride.value_or("")
                      );
                      onChange();
                  },
              },
          }
    {}
};

struct TermiosSettings
{
    struct InputFlags
    {
        BoolSetting<true> IGNBRK;
        BoolSetting<true> BRKINT;
        BoolSetting<true> IGNPAR;
        BoolSetting<true> PARMRK;
        BoolSetting<true> INPCK;
        BoolSetting<true> ISTRIP;
        BoolSetting<true> INLCR;
        BoolSetting<true> IGNCR;
        BoolSetting<true> ICRNL;
        BoolSetting<true> IUCLC;
        BoolSetting<true> IXON;
        BoolSetting<true> IXANY;
        BoolSetting<true> IXOFF;
        BoolSetting<true> IMAXBEL;
        BoolSetting<true> IUTF8;
    } inputFlags;

    struct OutputFlags
    {

        BoolSetting<true> OPOST;
        BoolSetting<true> OLCUC;
        BoolSetting<true> ONLCR;
        BoolSetting<true> OCRNL;
        BoolSetting<true> ONOCR;
        BoolSetting<true> ONLRET;
        BoolSetting<true> OFILL;
        BoolSetting<true> OFDEL;
        TextSetting<true> NLDLY;
        TextSetting<true> CRDLY;
        TextSetting<true> TABDLY;
        TextSetting<true> BSDLY;
        TextSetting<true> VTDLY;
        TextSetting<true> FFDLY;
    } outputFlags;

    Nui::Observed<std::string> groupKey{"default"};
    Nui::Observed<std::vector<std::string>> groupKeys{{"default"}};

    TermiosSettings(std::invocable auto const& onChange)
        : inputFlags{
              .IGNBRK{
                  language->getObserved("settings", "termios", "inputFlags", "IGNBRKHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IGNBRK.value(Persistence::Termios::InputFlags::saneDefaults().IGNBRK_);
                      onChange();
                  }
              },
              .BRKINT{
                  language->getObserved("settings", "termios", "inputFlags", "BRKINTHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.BRKINT.value(Persistence::Termios::InputFlags::saneDefaults().BRKINT_);
                      onChange();
                  }
              },
              .IGNPAR{
                  language->getObserved("settings", "termios", "inputFlags", "IGNPARHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IGNPAR.value(Persistence::Termios::InputFlags::saneDefaults().IGNPAR_);
                      onChange();
                  }
              },
              .PARMRK{
                  language->getObserved("settings", "termios", "inputFlags", "PARMRKHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.PARMRK.value(Persistence::Termios::InputFlags::saneDefaults().PARMRK_);
                      onChange();
                  }
              },
              .INPCK{
                  language->getObserved("settings", "termios", "inputFlags", "INPCKHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.INPCK.value(Persistence::Termios::InputFlags::saneDefaults().INPCK_);
                      onChange();
                  }
              },
              .ISTRIP{
                  language->getObserved("settings", "termios", "inputFlags", "ISTRIPHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.ISTRIP.value(Persistence::Termios::InputFlags::saneDefaults().ISTRIP_);
                      onChange();
                  }
              },
              .INLCR{
                  language->getObserved("settings", "termios", "inputFlags", "INLCRHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.INLCR.value(Persistence::Termios::InputFlags::saneDefaults().INLCR_);
                      onChange();
                  }
              },
              .IGNCR{
                  language->getObserved("settings", "termios", "inputFlags", "IGNCRHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IGNCR.value(Persistence::Termios::InputFlags::saneDefaults().IGNCR_);
                      onChange();
                  }
              },
              .ICRNL{
                  language->getObserved("settings", "termios", "inputFlags", "ICRNLHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.ICRNL.value(Persistence::Termios::InputFlags::saneDefaults().ICRNL_);
                      onChange();
                  }
              },
              .IUCLC{
                  language->getObserved("settings", "termios", "inputFlags", "IUCLCHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IUCLC.value(Persistence::Termios::InputFlags::saneDefaults().IUCLC_);
                      onChange();
                  }
              },
              .IXON{
                  language->getObserved("settings", "termios", "inputFlags", "IXONHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IXON.value(Persistence::Termios::InputFlags::saneDefaults().IXON_);
                      onChange();
                  }
              },
              .IXANY{
                  language->getObserved("settings", "termios", "inputFlags", "IXANYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IXANY.value(Persistence::Termios::InputFlags::saneDefaults().IXANY_);
                      onChange();
                  }
              },
              .IXOFF{
                  language->getObserved("settings", "termios", "inputFlags", "IXOFFHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IXOFF.value(Persistence::Termios::InputFlags::saneDefaults().IXOFF_);
                      onChange();
                  }
              },
              .IMAXBEL{
                  language->getObserved("settings", "termios", "inputFlags", "IMAXBELHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IMAXBEL.value(Persistence::Termios::InputFlags::saneDefaults().IMAXBEL_);
                      onChange();
                  }
              },
              .IUTF8{
                  language->getObserved("settings", "termios", "inputFlags", "IUTF8HelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      inputFlags.IUTF8.value(Persistence::Termios::InputFlags::saneDefaults().IUTF8_);
                      onChange();
                  }
              },
          }
        , outputFlags{
              .OPOST{
                  language->getObserved("settings", "termios", "outputFlags", "OPOSTHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.OPOST.value(Persistence::Termios::OutputFlags::saneDefaults().OPOST_);
                      onChange();
                  }
              },
              .OLCUC{
                  language->getObserved("settings", "termios", "outputFlags", "OLCUCHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.OLCUC.value(Persistence::Termios::OutputFlags::saneDefaults().OLCUC_);
                      onChange();
                  }
              },
              .ONLCR{
                  language->getObserved("settings", "termios", "outputFlags", "ONLCRHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.ONLCR.value(Persistence::Termios::OutputFlags::saneDefaults().ONLCR_);
                      onChange();
                  }
              },
              .OCRNL{
                  language->getObserved("settings", "termios", "outputFlags", "OCRNLHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.OCRNL.value(Persistence::Termios::OutputFlags::saneDefaults().OCRNL_);
                      onChange();
                  }
              },
              .ONOCR{
                  language->getObserved("settings", "termios", "outputFlags", "ONOCRHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.ONOCR.value(Persistence::Termios::OutputFlags::saneDefaults().ONOCR_);
                      onChange();
                  }
              },
              .ONLRET{
                  language->getObserved("settings", "termios", "outputFlags", "ONLRETHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.ONLRET.value(Persistence::Termios::OutputFlags::saneDefaults().ONLRET_);
                      onChange();
                  }
              },
              .OFILL{
                  language->getObserved("settings", "termios", "outputFlags", "OFILLHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.OFILL.value(Persistence::Termios::OutputFlags::saneDefaults().OFILL_);
                      onChange();
                  }
              },
              .OFDEL{
                  language->getObserved("settings", "termios", "outputFlags", "OFDELHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.OFDEL.value(Persistence::Termios::OutputFlags::saneDefaults().OFDEL_);
                      onChange();
                  }
              },
              .NLDLY{
                  language->getObserved("settings", "termios", "outputFlags", "NLDLYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.NLDLY.value(Persistence::Termios::OutputFlags::saneDefaults().NLDLY_.value_or(""));
                      onChange();
                  }
              },
              .CRDLY{
                  language->getObserved("settings", "termios", "outputFlags", "CRDLYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.CRDLY.value(Persistence::Termios::OutputFlags::saneDefaults().CRDLY_.value_or(""));
                      onChange();
                  }
              },
              .TABDLY{
                  language->getObserved("settings", "termios", "outputFlags", "TABDLYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.TABDLY.value(Persistence::Termios::OutputFlags::saneDefaults().TABDLY_.value_or(""));
                      onChange();
                  }
              },
              .BSDLY{
                  language->getObserved("settings", "termios", "outputFlags", "BSDLYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.BSDLY.value(Persistence::Termios::OutputFlags::saneDefaults().BSDLY_.value_or(""));
                      onChange();
                  }
              },
              .VTDLY{
                  language->getObserved("settings", "termios", "outputFlags", "VTDLYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.VTDLY.value(Persistence::Termios::OutputFlags::saneDefaults().VTDLY_.value_or(""));
                      onChange();
                  }
              },
              .FFDLY{
                  language->getObserved("settings", "termios", "outputFlags", "FFDLYHelpText"),
                  onChange,
                  [this, onChange]()
                  {
                      outputFlags.FFDLY.value(Persistence::Termios::OutputFlags::saneDefaults().FFDLY_.value_or(""));
                      onChange();
                  }
              },
          }
    {}
};

struct Settings::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;
    NewSessionDialog newSessionDialog{"settings"};
    Nui::ThrottledFunction throttledSave{};
    Nui::Observed<Settings::Section> activeSection{Settings::Section::GeneralSettings};
    Nui::Observed<std::optional<std::string>> activeSession{};
    Nui::Observed<bool> saveInProgress{false};

    Nui::Observed<std::vector<Settings::SectionSelectorOptions>> sessionSelectors{{
        {.sessionId = "Session 1", .icon = "it-system"},
        {.sessionId = "Session 2", .icon = "it-system"},
        {.sessionId = "Session 3", .icon = "it-system"},
    }};

    GeneralSettings generalSettings;
    TermiosSettings termiosSettings;

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

            const auto initialKeyTermios = [this]()
            {
                if (impl_->stateHolder->stateCache().termios.empty())
                    return "default"s;
                auto findDefault = impl_->stateHolder->stateCache().termios.find("default");
                if (findDefault != impl_->stateHolder->stateCache().termios.end())
                    return "default"s;
                return impl_->stateHolder->stateCache().termios.begin()->first;
            }();
            impl_->termiosSettings.groupKey = initialKeyTermios;
            impl_->termiosSettings.groupKeys = [&state = impl_->stateHolder->stateCache()]()
            {
                std::vector<std::string> keys;
                keys.reserve(state.termios.size());
                for (auto const& [key, _] : state.termios)
                    keys.push_back(key);
                return keys;
            }();
            loadTermiosSettingsFromStateByKey(initialKeyTermios, impl_->stateHolder->stateCache());

            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void Settings::loadTermiosSettingsFromStateByKey(std::string const& key, Persistence::State const& state)
{
    if (!state.termios.contains(key))
        return;

    impl_->termiosSettings.inputFlags.IGNBRK.value(state.termios.at(key).inputFlags.IGNBRK_);
    impl_->termiosSettings.inputFlags.BRKINT.value(state.termios.at(key).inputFlags.BRKINT_);
    impl_->termiosSettings.inputFlags.IGNPAR.value(state.termios.at(key).inputFlags.IGNPAR_);
    impl_->termiosSettings.inputFlags.PARMRK.value(state.termios.at(key).inputFlags.PARMRK_);
    impl_->termiosSettings.inputFlags.INPCK.value(state.termios.at(key).inputFlags.INPCK_);
    impl_->termiosSettings.inputFlags.ISTRIP.value(state.termios.at(key).inputFlags.ISTRIP_);
    impl_->termiosSettings.inputFlags.INLCR.value(state.termios.at(key).inputFlags.INLCR_);
    impl_->termiosSettings.inputFlags.IGNCR.value(state.termios.at(key).inputFlags.IGNCR_);
    impl_->termiosSettings.inputFlags.ICRNL.value(state.termios.at(key).inputFlags.ICRNL_);
    impl_->termiosSettings.inputFlags.IUCLC.value(state.termios.at(key).inputFlags.IUCLC_);
    impl_->termiosSettings.inputFlags.IXON.value(state.termios.at(key).inputFlags.IXON_);
    impl_->termiosSettings.inputFlags.IXANY.value(state.termios.at(key).inputFlags.IXANY_);
    impl_->termiosSettings.inputFlags.IXOFF.value(state.termios.at(key).inputFlags.IXOFF_);
    impl_->termiosSettings.inputFlags.IMAXBEL.value(state.termios.at(key).inputFlags.IMAXBEL_);
    impl_->termiosSettings.inputFlags.IUTF8.value(state.termios.at(key).inputFlags.IUTF8_);

    impl_->termiosSettings.outputFlags.OPOST.value(state.termios.at(key).outputFlags.OPOST_);
    impl_->termiosSettings.outputFlags.OLCUC.value(state.termios.at(key).outputFlags.OLCUC_);
    impl_->termiosSettings.outputFlags.ONLCR.value(state.termios.at(key).outputFlags.ONLCR_);
    impl_->termiosSettings.outputFlags.OCRNL.value(state.termios.at(key).outputFlags.OCRNL_);
    impl_->termiosSettings.outputFlags.ONOCR.value(state.termios.at(key).outputFlags.ONOCR_);
    impl_->termiosSettings.outputFlags.ONLRET.value(state.termios.at(key).outputFlags.ONLRET_);
    impl_->termiosSettings.outputFlags.OFILL.value(state.termios.at(key).outputFlags.OFILL_);
    impl_->termiosSettings.outputFlags.OFDEL.value(state.termios.at(key).outputFlags.OFDEL_);
    impl_->termiosSettings.outputFlags.NLDLY.value(state.termios.at(key).outputFlags.NLDLY_);
    impl_->termiosSettings.outputFlags.CRDLY.value(state.termios.at(key).outputFlags.CRDLY_);
    impl_->termiosSettings.outputFlags.TABDLY.value(state.termios.at(key).outputFlags.TABDLY_);
    impl_->termiosSettings.outputFlags.BSDLY.value(state.termios.at(key).outputFlags.BSDLY_);
    impl_->termiosSettings.outputFlags.VTDLY.value(state.termios.at(key).outputFlags.VTDLY_);
    impl_->termiosSettings.outputFlags.FFDLY.value(state.termios.at(key).outputFlags.FFDLY_);
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
        .outputFlags = Persistence::Termios::OutputFlags{
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
    };
    state.termios[key] = termiosEntry;
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
            .isCollapsed = impl_->generalSettings.collapsibleStates.localization,
            .content = std::move(localization),
            .headerTitle = language->getObserved("settings", "generalSettings")
        }),
        group({
            .isCollapsed = impl_->generalSettings.collapsibleStates.loggingAndErrorReporting,
            .content = std::move(loggingAndErrorReporting),
            .headerTitle = language->getObserved("settings", "loggingAndErrorReportingGroupHeader")
        }),
        group({
            .isCollapsed = impl_->generalSettings.collapsibleStates.userInterface,
            .content = std::move(userInterface),
            .headerTitle = language->getObserved("settings", "userInterfaceGroupHeader")
        }),
        group({
            .isCollapsed = impl_->generalSettings.collapsibleStates.localFilesystemOptions,
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
    auto termios = fragment(
        span{}("InputFlags"),
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
        impl_->termiosSettings.inputFlags.IUTF8("IUTF8"),
        span{}("OutputFlags"),
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
            .isCollapsed = impl_->generalSettings.collapsibleStates.localization,
            .content = std::move(termios),
            .headerTitle = language->getObserved("settings", "termiosGroupName"),
            .currentGroupKey = &impl_->termiosSettings.groupKey,
            .groupKeys = &impl_->termiosSettings.groupKeys,
            .isInheritableGroup = true,
        })
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
                "padding-top"_style = params.currentGroupKey ? "0px" : "8px",
                "padding-bottom"_style = "8px",
            },
        }(
            groupKeyContainer(),
            std::move(params.content)
        )
    );
    // clang-format on
}