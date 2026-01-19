#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/input.hpp>

#include <nui/frontend/filesystem/file_dialog.hpp>
#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

enum class PathSettingType
{
    File,
    Directory
};

template <bool Disengageable = false>
class PathSetting : public Setting<Disengageable, std::filesystem::path>
{
  public:
    using SettingBase = Setting<Disengageable, std::filesystem::path>;
    using Type = PathSettingType;

    using SettingBase::state_;
    using SettingBase::inheritedState_;
    using SettingBase::inheritanceStatus_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;

    PathSetting(
        LanguageObservedText helpText,
        Type type,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction,
        Nui::Observed<bool>* externalDisengage = nullptr
    )
        : SettingBase{
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction),
              externalDisengage
          }
        , type_(type)
    {}

    void openDialog()
    {
        if (type_ == Type::Directory)
        {
            Nui::FileDialog::showDirectoryDialog(
                {// all are optional
                    .title = "Pick directory / file",
                    .defaultPath = "%userprofile%",
                    .filters = {},
                    .forcePath = false
                },
                [this](std::optional<std::vector<std::filesystem::path>> results)
                {
                    if (!results) // The dialog was closed without selecting a file
                        return;
                    if (results->empty()) // nothing was selected, but the dialog was closed with ok.
                        return;

                    // Use the first selected path
                    state_ = results->at(0).string();
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    onChange_();
                }
            );
        }
        else
        {
            Nui::FileDialog::showOpenDialog(
                {// all are optional
                    .title = "Pick directory / file",
                    .defaultPath = "%userprofile%",
                    .filters = {},
                    .forcePath = false,
                    .allowMultiSelect = false
                },
                [this](std::optional<std::vector<std::filesystem::path>> results)
                {
                    if (!results) // The dialog was closed without selecting a file
                        return;
                    if (results->empty()) // nothing was selected, but the dialog was closed with ok.
                        return;

                    // Use the first selected path
                    state_ = results->at(0).string();
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    onChange_();
                }
            );
        }
    }

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            div{
                class_ = "setting-path"
            }(
                ui5::input{
                    class_ = "setting-input",
                    "value"_prop = SettingBase::observedValueWithInheritance(),
                    observeEngagedToBool("disabled"_prop),
                    "change"_event = [this](Nui::val event){
                        state_ = event["target"]["value"].as<std::string>();
                        onChange_();
                    }
                }(),
                ui5::button{
                    "design"_prop = "Transparent",
                    "icon"_prop = "browse-folder",
                    observeEngagedToBool("disabled"_prop),
                    "click"_event = [this](){
                        openDialog();
                    }
                }()
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    Type type_;
};