#pragma once

#include <frontend/settings/setting.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/select.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

template <typename ValueType, typename TransformedType = ValueType>
class ComboSetting : public Setting<ValueType>
{
  public:
    using Setting<ValueType>::state_;
    using Setting<ValueType>::onChange_;
    using Setting<ValueType>::reset;
    using Setting<ValueType>::help;

    ComboSetting(
        Nui::Observed<ValueType>& state,
        std::vector<ValueType> availableStates,
        std::string helpText,
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
        : Setting<ValueType>{
              state,
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
        , availableStates_{std::move(availableStates)}
        , iconAccessor_{std::move(iconAccessor)}
        , transform_{std::forward<decltype(valueTransformer)>(valueTransformer)}
    {}

    Nui::ElementRenderer operator()(auto& label)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            ui5::label{
                style = "color: var(--sapTextColor); margin-right: 10px",
                "showColon"_prop = true
            }(label),
            ui5::select{
                "change"_event = [this](Nui::val event){
                    const auto index = static_cast<std::size_t>(event["detail"]["selectedOption"]["valueIndex"].as<int>());
                    if (index < 0 || index > availableStates_.size())
                    {
                        Log::error("ComboSetting: Selected index {} is out of bounds.", index);
                        return;
                    }
                    *state_ = availableStates_[index];
                    onChange_();
                },
                "value"_prop = observe(*state_).generate([this](auto const& value){
                    return transform_(value);
                })
            }(
                Nui::range(availableStates_),
                [this](auto index, ValueType const& value) {
                    return ui5::option{
                        "icon"_prop = iconAccessor_ ? iconAccessor_(value) : std::nullopt,
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