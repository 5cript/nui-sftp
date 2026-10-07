#pragma once

#include <frontend/settings/search/setting_identity.hpp>

#include <optional>
#include <string>
#include <vector>

/**
 * @brief Language keys for settings whose keys do not follow the "<name>" / "<name>HelpText" convention.
 */
struct SettingKeyOverrides
{
    /**
     * @brief Label key below "settings".
     */
    std::optional<SettingKeyPath> labelKey{};
    /**
     * @brief Help text key below "settings".
     */
    std::optional<SettingKeyPath> helpTextKey{};
};

/**
 * @brief Hands out the identities settings are constructed with. A factory carries where the settings it creates
 * live: their scope, the groups they are shown under and the session engine they apply to.
 */
class SettingFactory
{
  public:
    SettingFactory(SettingsSearchIndex* index, SettingScope scope);

    /**
     * @brief A factory whose settings are shown under one more group or subgroup title.
     *
     * @param titleKey The language key of the title, below "settings".
     */
    SettingFactory within(SettingKeyPath const& titleKey) const;

    /**
     * @brief A factory whose settings only apply to sessions of the given engine.
     */
    SettingFactory forEngine(Persistence::TerminalEngineType engineType) const;

    /**
     * @brief A factory whose settings belong to options that are selected by a group key.
     */
    SettingFactory forGroup(Nui::Observed<std::optional<std::string>>& groupKey) const;

    /**
     * @brief Creates the identity of one setting. The label key is the path itself, the help text key appends
     * "HelpText" and the tags key "SearchTags" to the last segment.
     *
     * @param path The path below "settings", like {"sshOptions", "noDelay"}.
     */
    SettingIdentity identity(SettingKeyPath path, SettingKeyOverrides const& overrides = {}) const;

    SettingScope scope() const;

  private:
    SettingsSearchIndex* index_;
    SettingScope scope_;
    std::vector<std::vector<std::string>> breadcrumb_{};
    std::optional<Persistence::TerminalEngineType> engineType_{};
    Nui::Observed<std::optional<std::string>>* groupKey_{nullptr};
};
