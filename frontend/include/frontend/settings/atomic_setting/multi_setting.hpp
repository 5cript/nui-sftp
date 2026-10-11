#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/settings/search/setting_factory.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/select.hpp>
#include <script-nui-components/style_variant.hpp>

#include <frontend/svgs/add.hpp>
#include <frontend/svgs/delete.hpp>
#include <frontend/svgs/favorite.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/span.hpp>
#include <nui/frontend/elements/fragment.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/class.hpp>
#include <nui/frontend/attributes/disabled.hpp>
#include <nui/frontend/attributes/on_click.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/title.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

/**
 * @brief The settings that edit one entry of a MultiSetting, usually a struct of atomic settings.
 *
 * Constructed with the factory for the identities of its settings, the change callback they report edits to and
 * an observed flag that is false while there is no entry to edit (see Setting's externalDisengage).
 */
template <typename T>
concept MultiSettingEntry = requires(
    T& settings,
    T const& constSettings,
    typename T::StateType& state,
    typename T::StateType const& constState
) {
    typename T::StateType;
    requires std::constructible_from<T, SettingFactory const&, std::function<void()> const&, Nui::Observed<bool>*>;
    { constSettings.applyToState(state) };
    { settings.loadFromState(constState) };
    { settings.render() } -> std::convertible_to<Nui::ElementRenderer>;
};

/**
 * @brief A list of entries that each consist of several settings, like the users of a server. One entry is picked in
 * a select with add, delete and make-default buttons, and the settings of the entry are shown below it. The first
 * entry is the default one.
 */
template <MultiSettingEntry EntrySettingsT>
class MultiSetting : public Setting<false, std::vector<typename EntrySettingsT::StateType>>
{
  public:
    using EntryState = typename EntrySettingsT::StateType;
    using SettingBase = Setting<false, std::vector<EntryState>>;
    using ValueType = typename SettingBase::ValueType;
    using OutfacingValueType = typename SettingBase::OutfacingValueType;

    using SettingBase::state_;
    using SettingBase::onChange_;

    /**
     * @brief Names an entry in the select.
     */
    using EntryLabel = std::function<std::string(EntryState const&)>;

    /**
     * @param identity The identity of the list itself.
     * @param entryFactory Creates the identities of the settings of an entry.
     * @param entryLabel Names an entry in the select.
     */
    MultiSetting(
        SettingIdentity identity,
        SettingFactory const& entryFactory,
        std::function<void()> onChange,
        std::function<void()> resetAction,
        EntryLabel entryLabel
    )
        : SettingBase{std::move(identity), std::move(onChange), std::move(resetAction)}
        , entryLabel_{std::move(entryLabel)}
        , entrySettings_{
              entryFactory,
              [this]()
              {
                  storeSelectedEntry();
              },
              &hasEntries_
          }
    {}

    void value(ValueType const& value) override
    {
        state_ = value;
        selected_ = 0u;
        refreshEntries();
        loadSelectedEntry();
    }
    OutfacingValueType value() const override
    {
        return state_.value();
    }
    void value(std::optional<ValueType> const& value) override
    {
        this->value(value.value_or(ValueType{}));
    }

    /**
     * @brief The settings of an entry, which edit the selected one.
     */
    EntrySettingsT& entrySettings()
    {
        return entrySettings_;
    }

    Nui::ElementRenderer renderRow(LanguageObservedText labelText) override
    {
        namespace Snc = ScriptNuiComponents;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        const auto noEntries = [this]() -> std::optional<bool>
        {
            return hasEntries_.value() ? std::nullopt : std::optional<bool>{true};
        };

        // clang-format off
        return Nui::Elements::fragment(
            div{class_ = "multi-setting-header"}(
                div{class_ = "setting-fixed"}(
                    span{style = "color: var(--color); margin-right: 10px"}(std::move(labelText))
                ),
                div{class_ = "multi-setting-controls"}(
                    Snc::select(Snc::SelectOptions<decltype(selected_), decltype(entryIndices_)>{
                        .activeOption = selected_,
                        .options = entryIndices_,
                        .attributes = {
                            disabled = Nui::observe(hasEntries_).generate(noEntries),
                        },
                        .onChange = [this](std::size_t index, auto const&)
                        {
                            select(index);
                        },
                        .activeRenderer = [this](auto const&) -> Nui::ElementRenderer
                        {
                            return span{}(Nui::observe(state_, selected_), [this]() -> std::string {
                                if (!hasEntries_.value())
                                    return language->get("settings", "multiSetting", "noEntries");
                                return entryLabel_(state_.value()[selected_.value()]);
                            });
                        },
                        .elementRenderer = [this](std::size_t index) -> Nui::ElementRenderer
                        {
                            if (index >= state_.value().size())
                                return Nui::nil();
                            return span{}(entryLabel_(state_.value()[index]));
                        },
                        .makeId = []()
                        {
                            return Nui::val::global("generateId")().as<std::string>();
                        },
                        .dontUpdateValue = true,
                    }),
                    Snc::button({
                        .text = language->get("settings", "multiSetting", "addEntry"),
                        .icon = GeneratedSvgs::add(),
                        .attributes = {
                            onClick = [this]()
                            {
                                addEntry();
                            },
                        },
                        .styleVariant = Snc::StyleVariant::Transparent,
                    }),
                    Snc::button({
                        .text = language->get("settings", "multiSetting", "makeDefault"),
                        .icon = GeneratedSvgs::favorite(),
                        .attributes = {
                            title = language->get("settings", "multiSetting", "makeDefaultHelpText"),
                            disabled = Nui::observe(hasEntries_, selected_).generate([this]() -> std::optional<bool> {
                                return hasEntries_.value() && selected_.value() > 0u ? std::nullopt : std::optional<bool>{true};
                            }),
                            onClick = [this]()
                            {
                                makeSelectedDefault();
                            },
                        },
                        .styleVariant = Snc::StyleVariant::Transparent,
                    }),
                    Snc::button({
                        .text = language->get("settings", "multiSetting", "deleteEntry"),
                        .icon = GeneratedSvgs::delete_(),
                        .attributes = {
                            disabled = Nui::observe(hasEntries_).generate(noEntries),
                            onClick = [this]()
                            {
                                deleteSelectedEntry();
                            },
                        },
                        .styleVariant = Snc::StyleVariant::Transparent,
                    })
                ),
                SettingBase::reset(),
                SettingBase::help()
            ),
            div{
                class_ = Nui::observe(hasEntries_).generate([](bool hasEntries) {
                    return hasEntries ? "multi-setting-entry" : "multi-setting-entry multi-setting-entry-empty";
                }),
            }(
                entrySettings_.render()
            )
        );
        // clang-format on
    }

  private:
    void select(std::size_t index)
    {
        if (index >= state_.value().size())
            return;
        selected_ = index;
        loadSelectedEntry();
    }

    void addEntry()
    {
        state_.value().push_back(EntryState{});
        selected_ = state_.value().size() - 1u;
        refreshEntries();
        loadSelectedEntry();
        onChange_();
    }

    void deleteSelectedEntry()
    {
        auto& entries = state_.value();
        if (selected_.value() >= entries.size())
            return;
        entries.erase(std::next(entries.begin(), static_cast<std::ptrdiff_t>(selected_.value())));
        selected_ = std::min(selected_.value(), entries.empty() ? 0u : entries.size() - 1u);
        refreshEntries();
        loadSelectedEntry();
        onChange_();
    }

    void makeSelectedDefault()
    {
        auto& entries = state_.value();
        const auto index = selected_.value();
        if (index == 0u || index >= entries.size())
            return;
        const auto begin = entries.begin();
        std::rotate(
            begin,
            std::next(begin, static_cast<std::ptrdiff_t>(index)),
            std::next(begin, static_cast<std::ptrdiff_t>(index) + 1)
        );
        selected_ = 0u;
        refreshEntries();
        onChange_();
    }

    /**
     * @brief Writes the edits of the entry settings into the selected entry.
     */
    void storeSelectedEntry()
    {
        if (selected_.value() >= state_.value().size())
            return;
        entrySettings_.applyToState(state_.value()[selected_.value()]);
        refreshEntries();
        onChange_();
    }

    void loadSelectedEntry()
    {
        if (selected_.value() < state_.value().size())
            entrySettings_.loadFromState(state_.value()[selected_.value()]);
        else
            entrySettings_.loadFromState(EntryState{});
    }

    /**
     * @brief Updates what the select shows after entries were added, removed or renamed.
     */
    void refreshEntries()
    {
        std::vector<std::size_t> indices(state_.value().size());
        std::iota(indices.begin(), indices.end(), 0u);
        hasEntries_ = !indices.empty();
        entryIndices_ = std::move(indices);
        state_.modifyNow();
        selected_.modifyNow();
    }

    EntryLabel entryLabel_;
    Nui::Observed<std::size_t> selected_{0u};
    Nui::Observed<std::vector<std::size_t>> entryIndices_{};
    Nui::Observed<bool> hasEntries_{false};
    EntrySettingsT entrySettings_;
};
