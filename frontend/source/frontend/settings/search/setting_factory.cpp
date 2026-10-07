#include <frontend/settings/search/setting_factory.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <utility>

namespace
{
    std::vector<std::string> belowSettings(SettingKeyPath const& path)
    {
        std::vector<std::string> result{"settings"};
        result.insert(result.end(), path.begin(), path.end());
        return result;
    }

    std::vector<std::string> withSuffix(SettingKeyPath const& path, std::string_view suffix)
    {
        auto result = belowSettings(path);
        result.back() += suffix;
        return result;
    }
}

SettingFactory::SettingFactory(SettingsSearchIndex* index, SettingScope scope)
    : index_{index}
    , scope_{scope}
{}

SettingFactory SettingFactory::within(SettingKeyPath const& titleKey) const
{
    auto result = *this;
    result.breadcrumb_.push_back(belowSettings(titleKey));
    return result;
}

SettingFactory SettingFactory::forEngine(Persistence::TerminalEngineType engineType) const
{
    auto result = *this;
    result.engineType_ = engineType;
    return result;
}

SettingFactory SettingFactory::forGroup(Nui::Observed<std::optional<std::string>>& groupKey) const
{
    auto result = *this;
    result.groupKey_ = &groupKey;
    return result;
}

SettingIdentity SettingFactory::identity(SettingKeyPath path, SettingKeyOverrides const& overrides) const
{
    SettingIdentity result{};
    result.labelKey_ = belowSettings(overrides.labelKey.value_or(path));
    result.helpTextKey_ = overrides.helpTextKey ? belowSettings(*overrides.helpTextKey) : withSuffix(path, "HelpText");
    result.tagsKey_ = withSuffix(path, "SearchTags");
    result.breadcrumb_ = breadcrumb_;
    result.htmlId_ = fmt::format("setting-{}-{}", settingScopeName(scope_), fmt::join(path, "-"));
    result.scope_ = scope_;
    result.engineType_ = engineType_;
    result.groupKey_ = groupKey_;
    result.index_ = index_;
    result.path_ = std::move(path);
    return result;
}

SettingScope SettingFactory::scope() const
{
    return scope_;
}
