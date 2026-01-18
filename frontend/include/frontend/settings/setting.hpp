#pragma once

#include <utility/language.hpp>
#include <ids/id.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <traits/functions.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/responsive_popover.hpp>
#include <ui5/components/label.hpp>
#include <ui5/components/switch.hpp>
#include <ui5/components/check_box.hpp>

#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/reference.hpp>
#include <nui/frontend/attributes/id.hpp>
#include <nui/frontend/attributes/class.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/fragment.hpp>

#include <log/log.hpp>

#include <concepts>
#include <optional>

template <bool Disengageable, typename ValueType>
class Setting
{
  public:
    enum class InheritanceStatus
    {
        NoAncestor,
        AncestorEngaged,
        AncestorDisengaged
    };

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

    void value(ValueType const& value)
    {
        if constexpr (Disengageable)
        {
            engaged_ = true;
        }
        state_ = value;
    }
    OutfacingValueType value() const
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
    void value(std::optional<ValueType> const& value)
    {
        engaged_ = value.has_value();
        if (value)
            state_ = *value;
        else
            state_ = ValueType{};
    }
    auto observedValueWithInheritance()
    {
        return observe(engaged_, state_, inheritedState_, inheritanceStatus_)
            .generate(
                [](bool engaged,
                    ValueType const& value,
                    std::optional<ValueType> const& inheritedValue,
                    InheritanceStatus status)
                {
                    if (!engaged && inheritedValue && status == InheritanceStatus::AncestorEngaged)
                        return *inheritedValue;
                    return value;
                }
            );
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
        if (value)
        {
            inheritedState_ = value;
            inheritanceStatus(InheritanceStatus::AncestorEngaged);
        }
        else
        {
            inheritedState_ = std::nullopt;
            inheritanceStatus(InheritanceStatus::AncestorDisengaged);
        }
    }
    void inherit(ValueType const&) = delete;

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
        using Nui::Elements::div;

        if constexpr (!Disengageable)
        {
            return div{class_ = "setting-fixed"}(ui5::label{
                style = "color: var(--sapTextColor); margin-right: 10px", "showColon"_prop = true
            }(std::forward<decltype(label)>(label)));
        }
        else
        {
            return Nui::Elements::fragment(
                ui5::checkbox{
                    class_ = "setting-disengage-checkbox",
                    "checked"_prop = Nui::observe(engaged_).generate(
                        [](bool engaged)
                        {
                            return engaged;
                        }
                    ),
                    "change"_event =
                        [this](Nui::val event)
                    {
                        engaged_ = event["target"]["checked"].as<bool>();
                        onChange_();
                    }
                }(),
                div{class_ = "setting-disengageable"}(
                    ui5::label{
                        style = "color: var(--sapTextColor); margin-right: 10px", "showColon"_prop = true
                    }(std::forward<decltype(label)>(label)),
                    ui5::label{
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

        return ui5::button{
            "design"_prop = "Transparent",
            "icon"_prop = "refresh",
            "tooltip"_prop = language->getObserved("settings", "setting", "resetToDefaultValue"),
            "click"_event = [this]()
            {
                resetAction_();
            },
        }();
    }

    Nui::ElementRenderer help()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        const auto idString = Ids::generateId().id();

        return div{}(
            ui5::button{
                "design"_prop = "Transparent",
                "icon"_prop = "sys-help",
                id = idString,
                "click"_event =
                    [this]()
                {
                    // did it like this, because Observed<bool> looses track of the open status on clickoutside.
                    if (auto helpPopover = helpPopoverElement_.lock(); helpPopover)
                    {
                        helpPopover->val().set("open", !helpPopover->val()["open"].as<bool>());
                    }
                },
            }(),
            ui5::responsive_popover{
                reference =
                    [this](std::weak_ptr<Nui::Dom::BasicElement> const& ptr)
                {
                    helpPopoverElement_ = ptr;
                },
                "opener"_prop = idString,
                "header-text"_prop = "Help"
            }(helpText_)
        );
    }

  protected:
    Nui::Observed<ValueType> state_;
    Nui::Observed<std::optional<ValueType>> inheritedState_;
    Nui::Observed<bool> engaged_{!Disengageable};
    std::function<void()> onChange_;
    std::function<void()> resetAction_;
    Nui::Observed<bool>* externalDisengage_;
    LanguageObservedText helpText_;
    Nui::Observed<InheritanceStatus> inheritanceStatus_{InheritanceStatus::NoAncestor};

  private:
    std::weak_ptr<Nui::Dom::BasicElement> helpPopoverElement_;
};

template <typename ValueType>
class Setting<true, std::optional<ValueType>> : public Setting<true, ValueType>
{};

template <typename ValueType>
class Setting<false, std::optional<ValueType>> : public Setting<true, ValueType>
{};