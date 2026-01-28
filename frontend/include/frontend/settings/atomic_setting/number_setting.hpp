#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>

#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/elements/input.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/type.hpp>
#include <nui/frontend/attributes/max.hpp>
#include <nui/frontend/attributes/min.hpp>
#include <nui/frontend/attributes/step.hpp>
#include <nui/frontend/attributes/value.hpp>
#include <nui/frontend/attributes/pattern.hpp>

#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>

template <typename ValueType, bool Disengageable = false>
class NumberSetting : public Setting<Disengageable, ValueType>
{
  public:
    using SettingBase = Setting<Disengageable, ValueType>;

    using SettingBase::state_;
    using SettingBase::engaged_;
    using SettingBase::inheritedState_;
    using SettingBase::inheritanceStatus_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;

    enum class NumberBase
    {
        Decimal,
        Hexadecimal,
        Octal
    };

    struct ConstructionArgs
    {
        std::optional<ValueType> minValue{std::nullopt};
        std::optional<ValueType> maxValue{std::nullopt};
        std::optional<ValueType> stepValue{std::nullopt};
        std::optional<NumberBase> numberBase{std::nullopt};
        bool asRangeType = false;
    };

    explicit NumberSetting(
        LanguageObservedText helpText,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction,
        ConstructionArgs&& args,
        Nui::Observed<bool>* externalDisengage = nullptr
    )
        : SettingBase(
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction),
              externalDisengage
          )
        , args_(std::move(args))
    {}

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::input;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            div{
                class_ = "number-input-container"
            }(
                [this]() -> Nui::ElementRenderer {
                    if (args_.asRangeType)
                        return div{class_ = "setting-number-range-container"}(state_);
                    return Nui::nil();
                }(),
                inputElement()
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    ValueType retrieveValueFromEvent(Nui::val event)
    {
        if (args_.numberBase)
        {
            switch (*args_.numberBase)
            {
                case NumberBase::Decimal:
                {
                    const auto valueString = event["target"]["value"].as<std::string>();
                    ValueType result;
                    std::stringstream ss(valueString);
                    ss >> result;
                    return result;
                }
                case NumberBase::Hexadecimal:
                {
                    const auto valueStr = event["target"]["value"].as<std::string>();
                    return static_cast<ValueType>(std::stoul(valueStr, nullptr, 16));
                }
                case NumberBase::Octal:
                {
                    const auto valueStr = event["target"]["value"].as<std::string>();
                    if (valueStr.starts_with("0o") || valueStr.starts_with("0O"))
                        return static_cast<ValueType>(std::stoul(valueStr.substr(2), nullptr, 8));
                    return static_cast<ValueType>(std::stoul(valueStr, nullptr, 8));
                }
            }
        }
        return static_cast<ValueType>(event["target"]["value"].as<ValueType>());
    }

    std::string valueToString(ValueType value)
    {
        if (args_.numberBase)
        {
            switch (*args_.numberBase)
            {
                case NumberBase::Decimal:
                    return std::to_string(value);
                case NumberBase::Hexadecimal:
                {
                    std::stringstream ss;
                    ss << std::hex << value;
                    return ss.str();
                }
                case NumberBase::Octal:
                {
                    std::stringstream ss;
                    ss << std::oct << value;
                    return ss.str();
                }
            }
        }
        return std::to_string(value);
    }

    Nui::ElementRenderer inputElement()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::input;

        std::string type = "number";
        std::optional<std::string> validationPattern;
        if (args_.numberBase)
        {
            switch (*args_.numberBase)
            {
                case NumberBase::Decimal:
                    type = "number";
                    break;
                case NumberBase::Hexadecimal:
                    type = "text";
                    validationPattern = "0[xX][0-9a-fA-F]+";
                    break;
                case NumberBase::Octal:
                    type = "text";
                    validationPattern = "(0[0-7]+)|(0[oO][0-7]+)";
                    break;
            }
        }
        if (args_.asRangeType)
            type = "range";

        return input{
            class_ = "setting-number-input",
            observeEngagedToBool("disabled"_attr),
            Nui::Attributes::type = type,
            min = args_.minValue,
            max = args_.maxValue,
            step = args_.stepValue,
            pattern = validationPattern,
            value = Nui::observe(engaged_, state_, inheritedState_, inheritanceStatus_)
                .generate(
                    [this](
                        bool engaged,
                        ValueType const& value,
                        std::optional<ValueType> const& inheritedValue,
                        SettingBase::InheritanceStatus status
                    )
                    {
                        if (!engaged && inheritedValue && status == SettingBase::InheritanceStatus::AncestorEngaged)
                            return valueToString(*inheritedValue);
                        return valueToString(value);
                    }
                ),
            "keyup"_event =
                [this, type](Nui::WebApi::KeyboardEvent event)
            {
                const auto valueUnsanitized = retrieveValueFromEvent(event.val());
                const auto sanitized = std::clamp(
                    valueUnsanitized,
                    args_.minValue.value_or(std::numeric_limits<ValueType>::lowest()),
                    args_.maxValue.value_or(std::numeric_limits<ValueType>::max())
                );
                event.target().set("value", valueToString(sanitized));
            },
            "change"_event =
                [this](Nui::val event)
            {
                const auto valueUnsanitized = retrieveValueFromEvent(event);
                state_ = std::clamp(
                    valueUnsanitized,
                    args_.minValue.value_or(std::numeric_limits<ValueType>::lowest()),
                    args_.maxValue.value_or(std::numeric_limits<ValueType>::max())
                );
                onChange_();
            },
        }();
    }

  private:
    ConstructionArgs args_;
};