#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/text_input.hpp>

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
    using SettingBase::stateWithInheritance_;
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
    ValueType convertValue(std::string const& valueString)
    {
        try
        {
            switch (args_.numberBase.value_or(NumberBase::Decimal))
            {
                default:
                    [[fallthrough]];
                case NumberBase::Decimal:
                {
                    ValueType result;
                    std::stringstream ss(valueString);
                    ss >> result;
                    return result;
                }
                case NumberBase::Hexadecimal:
                {
                    return static_cast<ValueType>(std::stoul(valueString, nullptr, 16));
                }
                case NumberBase::Octal:
                {
                    if (valueString.starts_with("0o") || valueString.starts_with("0O"))
                        return static_cast<ValueType>(std::stoul(valueString.substr(2), nullptr, 8));
                    return static_cast<ValueType>(std::stoul(valueString, nullptr, 8));
                }
            }
        }
        catch (const std::exception& e)
        {
            Log::error("Failed to parse number from input event: {}", e.what());
            return ValueType{};
        }
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

        std::string type = "text";
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
        // TODO: Reintroduce
        // if (args_.asRangeType)
        //     type = "range";

        return ScriptNuiComponents::textInput(
            ScriptNuiComponents::TextInputOptions<decltype(stateWithInheritance_)>{
                .value = stateWithInheritance_,
                .attributes =
                    {
                        observeEngagedToBool(disabled),
                        pattern = validationPattern,
                        Nui::Attributes::type = type,
                    },
                .onChange =
                    [this, type](auto const& state, Nui::WebApi::Event const& event)
                {
                    const auto valueUnsanitized = convertValue(state);
                    this->value(
                        std::clamp(
                            valueUnsanitized,
                            args_.minValue.value_or(std::numeric_limits<ValueType>::lowest()),
                            args_.maxValue.value_or(std::numeric_limits<ValueType>::max())
                        )
                    );
                    this->valueIsValid_ = !event.target()["validity"]["patternMismatch"].as<bool>();
                    onChange_();
                },
                .dontUpdateValue = true
            }
        );
    }

  private:
    ConstructionArgs args_;
};