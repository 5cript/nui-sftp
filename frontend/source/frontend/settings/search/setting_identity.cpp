#include <frontend/settings/search/setting_identity.hpp>

std::string_view settingScopeName(SettingScope scope)
{
    switch (scope)
    {
        case SettingScope::General:
            return "general";
        case SettingScope::Inheritable:
            return "inheritable";
        case SettingScope::Session:
            return "session";
    }
    return "unknown";
}

SettingKeyPath const& SettingIdentity::path() const
{
    return path_;
}

std::vector<std::string> const& SettingIdentity::labelKey() const
{
    return labelKey_;
}

std::vector<std::string> const& SettingIdentity::helpTextKey() const
{
    return helpTextKey_;
}

std::vector<std::string> const& SettingIdentity::tagsKey() const
{
    return tagsKey_;
}

std::vector<std::vector<std::string>> const& SettingIdentity::breadcrumb() const
{
    return breadcrumb_;
}

std::string const& SettingIdentity::htmlId() const
{
    return htmlId_;
}

SettingScope SettingIdentity::scope() const
{
    return scope_;
}

std::optional<Persistence::TerminalEngineType> const& SettingIdentity::engineType() const
{
    return engineType_;
}

Nui::Observed<std::optional<std::string>>* SettingIdentity::groupKey() const
{
    return groupKey_;
}

SettingsSearchIndex* SettingIdentity::index() const
{
    return index_;
}

LanguageObservedText SettingIdentity::observedLabel() const
{
    return language->getObservedByPath(labelKey_, path_.empty() ? std::string{} : path_.back());
}

LanguageObservedText SettingIdentity::observedHelpText() const
{
    return language->getObservedByPath(helpTextKey_, std::string{});
}
