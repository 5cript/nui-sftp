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

#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/reference.hpp>
#include <nui/frontend/attributes/id.hpp>
#include <nui/frontend/attributes/class.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/elements/div.hpp>

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

    Setting(LanguageObservedText helpText, std::invocable auto&& onChange, std::invocable auto&& resetAction)
        : state_{}
        , onChange_{std::forward<decltype(onChange)>(onChange)}
        , resetAction_{std::forward<decltype(resetAction)>(resetAction)}
        , helpText_{std::move(helpText)}
    {}
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

    Nui::ElementRenderer disengageable()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if constexpr (Disengageable)
        {
            // clang-format off
            return div{
                class_ = "setting-disengageable"
            }(
                ui5::switch_{
                    "checked"_prop = Nui::observe(engaged_).generate([](bool engaged){
                        return engaged;
                    }),
                    "change"_event = [this](Nui::val event){
                        bool engaged = event["target"]["checked"].as<bool>();
                        engaged_ = engaged;
                        onChange_();
                    }
                }(),
                ui5::label{
                    style = "color: var(--subduedText);",
                }(observe(engaged_, inheritanceStatus_).generate([](bool engaged, InheritanceStatus status) {
                    if (!engaged) {
                        switch (status) {
                            case InheritanceStatus::NoAncestor:
                            case InheritanceStatus::AncestorDisengaged:
                                return language->get("settings", "setting", "settingInactive");
                            case InheritanceStatus::AncestorEngaged:
                                return language->get("settings", "setting", "settingInherits");
                        }
                    } else {
                        switch (status) {
                            case InheritanceStatus::NoAncestor:
                            case InheritanceStatus::AncestorDisengaged:
                                return language->get("settings", "setting", "settingActive");
                            case InheritanceStatus::AncestorEngaged:
                                return language->get("settings", "setting", "settingOverrides");
                        }
                    }
                }))
            );
            // clang-format on
        }
        else
        {
            return div{style = "visibility: hidden;"}();
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
    Nui::Observed<bool> engaged_{!Disengageable};
    std::function<void()> onChange_;
    std::function<void()> resetAction_;
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