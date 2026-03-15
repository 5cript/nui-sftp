#pragma once

#include <frontend/classes.hpp>
#include <utility/language.hpp>
#include <ids/id.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <traits/functions.hpp>

#include <frontend/svgs/refresh.hpp>
#include <frontend/svgs/question-mark.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/text_input.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/style_variant.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/reference.hpp>
#include <nui/frontend/attributes/id.hpp>
#include <nui/frontend/attributes/class.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/type.hpp>
#include <nui/frontend/attributes/checked.hpp>
#include <nui/frontend/attributes/on_click.hpp>
#include <nui/frontend/attributes/alt.hpp>
#include <nui/frontend/attributes/disabled.hpp>
#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/span.hpp>
#include <nui/frontend/elements/input.hpp>
#include <nui/frontend/elements/button.hpp>
#include <nui/frontend/elements/fragment.hpp>

#include <log/log.hpp>

#include <concepts>
#include <optional>

template <bool Disengageable, typename ValueT>
class Setting
{
  public:
    enum class InheritanceStatus
    {
        NoAncestor,
        AncestorEngaged,
        AncestorDisengaged
    };

    using ValueType = ValueT;
    using OutfacingValueType = std::conditional_t<Disengageable, std::optional<ValueType>, ValueType>;

    Setting(
        LanguageObservedText helpText,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction,
        Nui::Observed<bool>* externalDisengage = nullptr
    )
        : state_{}
        , onChange_{std::forward<decltype(onChange)>(onChange)}
        , resetAction_{std::forward<decltype(resetAction)>(resetAction)}
        , externalDisengage_{externalDisengage}
        , helpText_{std::move(helpText)}
    {
        if (!onChange_)
        {
            Log::error("Setting: Invalid onChange callable provided.");
            throw std::invalid_argument("Setting: Invalid onChange callable provided.");
        }
        if (!resetAction_)
        {
            Log::error("Setting: Invalid resetAction callable provided.");
            throw std::invalid_argument("Setting: Invalid resetAction callable provided.");
        }
    }
    virtual ~Setting() = default;

    Nui::Observed<ValueType>& state()
    {
        return state_;
    }

    virtual void value(ValueType const& value)
    {
        if constexpr (Disengageable)
        {
            engaged_ = true;
        }
        state_ = value;
        updateStateWithInheritance();
    }
    virtual OutfacingValueType value() const
    {
        if constexpr (Disengageable)
        {
            if (isEngaged())
                return state_.value();
            return std::nullopt;
        }
        else
        {
            return state_.value();
        }
    }
    virtual void value(std::optional<ValueType> const& value)
    {
        engaged_ = value.has_value();
        if (value)
            this->value(*value);
        else
            updateStateWithInheritance();
    }
    void updateStateWithInheritance()
    {
        stateWithInheritance_ = [this]()
        {
            if (!engaged_.value() && inheritedState_.value() &&
                inheritanceStatus_.value() == InheritanceStatus::AncestorEngaged)
                return inheritedState_->value();
            return state_.value();
        }();
    }

    InheritanceStatus inheritanceStatus() const
    {
        return inheritanceStatus_.value();
    }
    void inheritanceStatus(InheritanceStatus status)
    {
        inheritanceStatus_ = status;
    }

    bool isEngaged() const
    {
        if constexpr (Disengageable)
            return engaged_.value();
        else
            return true;
    }
    void inherit(std::optional<ValueType> const& value)
    {
        inheritedState_ = value;
        if (value)
        {
            inheritanceStatus(InheritanceStatus::AncestorEngaged);
        }
        else
        {
            inheritanceStatus(InheritanceStatus::AncestorDisengaged);
        }
        updateStateWithInheritance();
    }
    void inherit(ValueType const&) = delete;
    // As opposed to inherit(value), because this is intentional:
    void inheritValue(ValueType const& value)
    {
        inheritedState_ = value;
        inheritanceStatus(InheritanceStatus::AncestorEngaged);
        updateStateWithInheritance();
    }

    auto observeEngagedToBool(auto&& prop)
    {
        if (externalDisengage_)
        {
            return prop = observe(engaged_, *externalDisengage_)
                              .generate(
                                  [](bool engaged, bool externalDisengage)
                                  {
                                      return !engaged || !externalDisengage;
                                  }
                              );
        }
        // Trick to arrive at same return type. To solve this we would need to turn it upside down and apply to an
        // element.
        return prop = observe(engaged_).generate(
                   [](bool engaged)
                   {
                       return !engaged;
                   }
               );
    }

    Nui::ElementRenderer label(auto&& label)
    {
        using namespace Nui::Attributes;
        using namespace Nui::Elements;
        using Nui::Elements::div;
        using Nui::Elements::span;

        if constexpr (!Disengageable)
        {
            return div{class_ = "setting-fixed"}(span{
                style = "color: var(--sapTextColor); margin-right: 10px", "showColon"_prop = true
            }(std::forward<decltype(label)>(label)));
        }
        else
        {
            return Nui::Elements::fragment(
                ScriptNuiComponents::switch_(
                    ScriptNuiComponents::SwitchOptions<decltype(engaged_)>{
                        .isChecked = engaged_,
                        .onChange =
                            [this](bool, Nui::WebApi::MouseEvent const&)
                        {
                            Nui::WebApi::Console::log(
                                "Setting: Engaged state changed to {}, triggering onChange.", engaged_.value()
                            );
                            updateStateWithInheritance();
                            onChange_();
                        }
                    }
                ),
                div{class_ = "setting-disengageable"}(
                    span{
                        style = "color: var(--sapTextColor); margin-right: 10px", "showColon"_prop = true
                    }(std::forward<decltype(label)>(label)),
                    span{
                        style = "color: var(--subduedText);",
                    }(observe(engaged_, inheritanceStatus_)
                            .generate(
                                [](bool engaged, InheritanceStatus status)
                                {
                                    if (!engaged)
                                    {
                                        switch (status)
                                        {
                                            case InheritanceStatus::NoAncestor:
                                            case InheritanceStatus::AncestorDisengaged:
                                                return language->get("settings", "setting", "settingInactive");
                                            case InheritanceStatus::AncestorEngaged:
                                                return language->get("settings", "setting", "settingInherits");
                                        }
                                    }
                                    else
                                    {
                                        switch (status)
                                        {
                                            case InheritanceStatus::NoAncestor:
                                            case InheritanceStatus::AncestorDisengaged:
                                                return language->get("settings", "setting", "settingActive");
                                            case InheritanceStatus::AncestorEngaged:
                                                return language->get("settings", "setting", "settingOverrides");
                                        }
                                    }
                                }
                            ))
                )
            );
        }
    }

    Nui::ElementRenderer reset()
    {
        using namespace Nui::Attributes;
        using namespace Nui::Elements;
        using namespace std::string_literals;

        return ScriptNuiComponents::button(
            ScriptNuiComponents::ButtonOptions<std::string>{
                .text = ""s,
                .icon = GeneratedSvgs::refresh(),
                .attributes =
                    {alt = language->getObserved("settings", "setting", "resetToDefaultValue"),
                        onClick =
                            [this]()
                        {
                            resetAction_();
                        }},
                .styleVariant = ScriptNuiComponents::StyleVariant::Transparent,
            }
        );
    }

    Nui::ElementRenderer help()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using namespace std::string_literals;

        const auto idString = Ids::generateId().id();

        return ScriptNuiComponents::button(
            ScriptNuiComponents::ButtonOptions<std::string>{
                .text = ""s,
                .icon = GeneratedSvgs::questionmark(),
                .attributes =
                    {alt = helpText_,
                        onClick =
                            [this]()
                        {
                            // TODO: Replace with proper popover instead of alert
                            Nui::val::global("alert")(helpText_.value());
                        }},
                .styleVariant = ScriptNuiComponents::StyleVariant::Transparent,
            }
        );
    }

    bool valueIsValid() const
    {
        return valueIsValid_;
    }

  protected:
    Nui::Observed<ValueType> state_;
    Nui::Observed<std::optional<ValueType>> inheritedState_;
    Nui::Observed<ValueType> stateWithInheritance_;
    Nui::Observed<bool> engaged_{!Disengageable};
    std::function<void()> onChange_;
    std::function<void()> resetAction_;
    Nui::Observed<bool>* externalDisengage_;
    LanguageObservedText helpText_;
    Nui::Observed<InheritanceStatus> inheritanceStatus_{InheritanceStatus::NoAncestor};
    bool valueIsValid_{true};
};

template <typename ValueType>
class Setting<true, std::optional<ValueType>> : public Setting<true, ValueType>
{};

template <typename ValueType>
class Setting<false, std::optional<ValueType>> : public Setting<true, ValueType>
{};