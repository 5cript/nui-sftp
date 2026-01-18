#pragma once

#include <frontend/settings/setting.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/select.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

template <typename ValueType, typename TransformedType = ValueType, bool Disengageable = false>
class ComboSetting : public Setting<Disengageable, ValueType>
{
  public:
    using SettingBase = Setting<Disengageable, ValueType>;
    using SettingBase::state_;
    using SettingBase::inheritedState_;
    using SettingBase::inheritanceStatus_;
    using SettingBase::engaged_;

    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;

    ComboSetting(
        std::vector<ValueType> availableStates,
        LanguageObservedText helpText,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction,
        Traits::Callable auto&& valueTransformer =
            [](ValueType const& v)
        {
            return v;
        },
        std::function<std::optional<std::string>(ValueType const&)> iconAccessor =
            [](ValueType const&)
        {
            return std::nullopt;
        }
    )
        : SettingBase{
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
        , availableStates_{std::move(availableStates)}
        , iconAccessor_{std::move(iconAccessor)}
        , transform_{std::forward<decltype(valueTransformer)>(valueTransformer)}
    {
        if (!transform_)
        {
            Log::error("ComboSetting: Invalid value transformer provided.");
            throw std::invalid_argument("Invalid value transformer provided to ComboSetting.");
        }
        if (!iconAccessor_)
        {
            Log::error("ComboSetting: Invalid icon accessor provided.");
            throw std::invalid_argument("Invalid icon accessor provided to ComboSetting.");
        }
    }

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            ui5::select{
                "change"_event = [this](Nui::val event){
                    const auto index = static_cast<std::size_t>(event["detail"]["selectedOption"]["valueIndex"].as<int>());
                    if (index < 0 || index > availableStates_.size())
                    {
                        Log::error("ComboSetting: Selected index {} is out of bounds.", index);
                        return;
                    }
                    state_ = availableStates_[index];
                    onChange_();
                },
                SettingBase::observeEngagedToBool("disabled"_prop),
                "value"_prop = observe(engaged_, state_, inheritedState_, inheritanceStatus_).generate(
                    [this]() {
                        try {
                            if (!*engaged_ && *inheritedState_ && *inheritanceStatus_ == SettingBase::InheritanceStatus::AncestorEngaged)
                                return transform_(**inheritedState_);
                            return transform_(*state_);
                        } catch (std::exception const& e) {
                            Log::error("ComboSetting: Exception in value generation: {}", e.what());
                            return TransformedType{};
                        }
                    }
                )
            }(
                Nui::range(availableStates_),
                [this](auto index, ValueType const& value) {
                    return ui5::option{
                        "icon"_prop = observe(state_).generate([this, value](){
                            return iconAccessor_ ? iconAccessor_(value) : std::nullopt;
                        }),
                        "valueIndex"_prop = static_cast<int>(index),
                    }(transform_(value));
                }
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    std::vector<ValueType> availableStates_;
    std::function<std::optional<std::string>(ValueType const&)> iconAccessor_;
    std::function<TransformedType(ValueType const&)> transform_;
};