#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <log/log.hpp>

#include <script-nui-components/select.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/fragment.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/class.hpp>

template <typename ValueType, typename TransformedType = ValueType, bool Disengageable = false>
class ComboSetting : public Setting<Disengageable, ValueType>
{
  public:
    using SettingBase = Setting<Disengageable, ValueType>;
    using SettingBase::stateWithInheritance_;
    using SettingBase::observeEngagedToBool;

    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::value;

    ComboSetting(
        std::vector<ValueType> availableStates,
        LanguageObservedText helpText,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction,
        std::function<TransformedType(ValueType const&)> valueTransformer = {},
        std::function<Nui::ElementRenderer(ValueType const&)> iconRenderer = {},
        std::function<bool()> doLoad = {}
    )
        : SettingBase{
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
        , availableStates_{std::move(availableStates)}
        , valueTransformer_{std::move(valueTransformer)}
        , iconRenderer_{std::move(iconRenderer)}
        , doLoad_{std::move(doLoad)}
    {}

    Nui::ElementRenderer renderActive()
    {
        using Nui::Elements::span;
        using Nui::Elements::div;
        using namespace Nui::Attributes;

        auto transform = [this]()
        {
            if (valueTransformer_)
                return valueTransformer_(stateWithInheritance_.value());
            else
            {
                if constexpr (std::is_same_v<ValueType, TransformedType>)
                    return stateWithInheritance_.value();
                else
                {
                    Nui::WebApi::Console::error(
                        "ComboSetting: No value transformer provided for different ValueType and TransformedType!"
                    );
                    return TransformedType{};
                }
            }
        };

        if (!iconRenderer_)
            return span{}(
                observe(stateWithInheritance_),
                [transform]()
                {
                    return transform();
                }
            );

        return div{
            class_ = "combo-setting-option"
        }(observe(stateWithInheritance_),
            [this, transform](ValueType const& option) -> Nui::ElementRenderer
            {
                return Nui::Elements::fragment(
                    iconRenderer_(option),
                    span{}(
                        [transform]()
                        {
                            return transform();
                        }
                    )
                );
            });
    }

    void options(std::vector<ValueType> const& newOptions)
    {
        availableStates_ = newOptions;
    }

    Nui::ElementRenderer renderOption(ValueType const& option)
    {
        using Nui::Elements::span;
        using Nui::Elements::div;
        using namespace Nui::Attributes;

        auto transform = [this, option]()
        {
            if (valueTransformer_)
                return valueTransformer_(option);
            else
            {
                if constexpr (std::is_same_v<ValueType, TransformedType>)
                    return option;
                else
                {
                    Nui::WebApi::Console::error(
                        "ComboSetting: No value transformer provided for different ValueType and TransformedType!"
                    );
                    return TransformedType{};
                }
            }
        };

        if (!iconRenderer_)
            return span{}(transform());

        return div{class_ = "combo-setting-option"}(iconRenderer_(option), span{}(transform()));
    }

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            ScriptNuiComponents::select(
                ScriptNuiComponents::SelectOptions<decltype(stateWithInheritance_), decltype(availableStates_)>{
                    .activeOption = stateWithInheritance_,
                    .options = availableStates_,
                    .attributes = {
                        observeEngagedToBool(disabled)
                    },
                    .onChange = [this](auto const& newValue, auto const&)
                    {
                        this->value(newValue);
                        onChange_();
                    },
                    .activeRenderer = [this](auto const&) -> Nui::ElementRenderer
                    {
                        return renderActive();
                    },
                    .elementRenderer = [this](auto const& option) -> Nui::ElementRenderer
                    {
                        return renderOption(option);
                    },
                    .makeId = [](){
                        return Nui::val::global("generateId")().as<std::string>();
                    },
                    .onOpen = [this]()
                    {
                        if (doLoad_)
                            return doLoad_();
                        return false;
                    },
                    .dontUpdateValue = true,
                }
            ),
            //div{}("ComboSetting not implemented yet"),// TODO implement select component and use it here instead of this placeholder
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    Nui::Observed<std::vector<ValueType>> availableStates_;
    std::function<TransformedType(ValueType const&)> valueTransformer_;
    std::function<Nui::ElementRenderer(ValueType const&)> iconRenderer_;
    std::function<bool()> doLoad_;
};