#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/confirm_dialog.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <nlohmann/json.hpp>

#include <map>
#include <string>

class LayoutSetting : public Setting<false, std::map<std::string, nlohmann::json>>
{
  public:
    using SettingBase = Setting<false, std::map<std::string, nlohmann::json>>;
    using ValueType = SettingBase::ValueType;

    LayoutSetting(
        LanguageObservedText helpText,
        std::function<void()> onChange,
        std::function<std::optional<nlohmann::json>()> obtainCurrentLayout,
        ConfirmDialog* confirmDialog,
        InputDialog* newItemDialog
    );

    Nui::ElementRenderer operator()();

    void value(ValueType const& value) override
    {
        state_ = value;
        engaged_ = true;
        if (!state_.value().empty())
        {
            if (!state_.value().contains(*selected_))
                selected_ = state_.value().begin()->first;
        }
        else
            selected_ = "";
    }
    OutfacingValueType value() const override
    {
        return state_.value();
    }
    void value(std::optional<ValueType> const& value) override
    {
        engaged_ = value.has_value();
        if (value)
            this->value(*value);
        else
            state_ = ValueType{};
    }

  private:
    ConfirmDialog* confirmDialog_;
    InputDialog* newItemDialog_;
    Nui::Observed<std::string> selected_;
    std::function<std::optional<nlohmann::json>()> obtainCurrentLayout_;
};