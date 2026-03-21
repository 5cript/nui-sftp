#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/text_input.hpp>
#include <script-nui-components/button.hpp>

#include <nui/frontend/filesystem/file_dialog.hpp>
#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/input.hpp>
#include <nui/frontend/elements/button.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/value.hpp>
#include <nui/frontend/attributes/disabled.hpp>
#include <nui/frontend/attributes/on_click.hpp>

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
    using SettingBase::stateWithInheritance_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::value;
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
                    // Print results:
                    if (results)
                    {
                        for (const auto& path : *results)
                        {
                            Log::info("Selected path: {}", path.string());
                        }
                    }
                    else
                    {
                        Log::info("Dialog was closed without selecting a path.");
                    }

                    if (!results) // The dialog was closed without selecting a file
                        return;
                    if (results->empty()) // nothing was selected, but the dialog was closed with ok.
                        return;

                    // Use the first selected path
                    SettingBase::value(results->at(0));
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
                    SettingBase::value(results->at(0));
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    onChange_();
                }
            );
        }
    }

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            div{
                class_ = "setting-path"
            }(
                ScriptNuiComponents::textInput({
                        .value = stateWithInheritance_,
                        .attributes = {observeEngagedToBool(disabled)},
                        .onChange = [this](auto const& state, Nui::WebApi::Event const&){
                            SettingBase::value(std::filesystem::path{state});
                            onChange_();
                        },
                        .dontUpdateValue = true,
                    }
                ),
                ScriptNuiComponents::button({
                    .text = language->get("settings", "pathSetting", "browseButton"),
                    .attributes = {
                        observeEngagedToBool(disabled),
                        onClick = [this](auto const&){
                            openDialog();
                        }
                    },
                    .styleVariant = ScriptNuiComponents::StyleVariant::Regular,
                })
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    Type type_;
};