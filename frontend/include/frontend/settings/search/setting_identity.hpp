#pragma once

#include <utility/language.hpp>
#include <persistence/state/session_options.hpp>

#include <nui/event_system/observed_value.hpp>

#include <optional>
#include <string>
#include <vector>

class SettingsSearchIndex;
class SettingFactory;

/**
 * @brief Where a setting lives on the settings page.
 */
enum class SettingScope
{
    General,
    Inheritable,
    Session
};

/**
 * @brief The name of a scope as used in html ids.
 */
std::string_view settingScopeName(SettingScope scope);

/**
 * @brief A key path into the language files, below "settings".
 */
using SettingKeyPath = std::vector<std::string>;

/**
 * @brief Everything that identifies one rendered setting: its language keys, its html id and where it lives.
 * Only a SettingFactory creates identities, so ids and keys always follow the same scheme.
 */
class SettingIdentity
{
  public:
    /**
     * @brief The path of the setting below "settings", like {"sshOptions", "noDelay"}.
     */
    SettingKeyPath const& path() const;

    /**
     * @brief Full language key of the label, including "settings".
     */
    std::vector<std::string> const& labelKey() const;

    /**
     * @brief Full language key of the help text, including "settings".
     */
    std::vector<std::string> const& helpTextKey() const;

    /**
     * @brief Full language key of the comma separated search tags, including "settings".
     */
    std::vector<std::string> const& tagsKey() const;

    /**
     * @brief Full language keys of the group and subgroup titles the setting is shown under.
     */
    std::vector<std::vector<std::string>> const& breadcrumb() const;

    /**
     * @brief The id of the element wrapping the rendered setting.
     */
    std::string const& htmlId() const;

    SettingScope scope() const;

    /**
     * @brief The session engine the setting applies to, if it is limited to one.
     */
    std::optional<Persistence::TerminalEngineType> const& engineType() const;

    /**
     * @brief The group selection of the options the setting belongs to, if they are inheritable groups.
     */
    Nui::Observed<std::optional<std::string>>* groupKey() const;

    SettingsSearchIndex* index() const;

    /**
     * @brief The label in the current language. Without a translation the last path segment is used, which is
     * what raw names like termios flags rely on.
     */
    LanguageObservedText observedLabel() const;

    /**
     * @brief The help text in the current language.
     */
    LanguageObservedText observedHelpText() const;

  private:
    friend class SettingFactory;

    SettingIdentity() = default;

    SettingKeyPath path_{};
    std::vector<std::string> labelKey_{};
    std::vector<std::string> helpTextKey_{};
    std::vector<std::string> tagsKey_{};
    std::vector<std::vector<std::string>> breadcrumb_{};
    std::string htmlId_{};
    SettingScope scope_{SettingScope::General};
    std::optional<Persistence::TerminalEngineType> engineType_{};
    Nui::Observed<std::optional<std::string>>* groupKey_{nullptr};
    SettingsSearchIndex* index_{nullptr};
};
