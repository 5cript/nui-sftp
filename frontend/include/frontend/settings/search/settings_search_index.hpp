#pragma once

#include <frontend/settings/search/setting_identity.hpp>
#include <utility/fuzzy_search.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * @brief All settings that can be found by the settings search. Settings register themselves on construction and
 * unregister on destruction, so the index always reflects the settings that exist. A setting registered in several
 * scopes, like inheritable options that sessions override, is one entry with several scopes.
 */
class SettingsSearchIndex
{
  public:
    /**
     * @brief One scope a setting is registered in.
     */
    struct ScopeRegistration
    {
        std::string htmlId{};
        std::vector<std::vector<std::string>> breadcrumb{};
        std::optional<Persistence::TerminalEngineType> engineType{};
        Nui::Observed<std::optional<std::string>>* groupKey{nullptr};
        bool rendered{false};
    };

    /**
     * @brief One searchable setting.
     */
    struct Entry
    {
        SettingKeyPath path{};
        std::vector<std::string> labelKey{};
        std::vector<std::string> helpTextKey{};
        std::vector<std::string> tagsKey{};
        std::map<SettingScope, ScopeRegistration> scopes{};

        /**
         * @brief Whether the setting is rendered in at least one scope.
         */
        bool isRendered() const;

        /**
         * @brief The registration of the scope, if the setting is rendered in it.
         */
        ScopeRegistration const* renderedScope(SettingScope scope) const;
    };

    /**
     * @brief A setting found by a search.
     */
    struct SearchResult
    {
        Entry const* entry{nullptr};
        Utility::FuzzySearch::RankedMatch match{};
        /**
         * @brief The tag that matched in the current language or English; empty unless the match is by tag.
         */
        std::string matchedTag{};
    };

    /**
     * @brief Keeps a setting registered for as long as it lives.
     */
    class Registration
    {
      public:
        explicit Registration(SettingIdentity const& identity);
        ~Registration();
        Registration(Registration const&) = delete;
        Registration(Registration&&) = delete;
        Registration& operator=(Registration const&) = delete;
        Registration& operator=(Registration&&) = delete;

        /**
         * @brief Records that the setting was rendered, which makes it searchable.
         */
        void markRendered();

      private:
        SettingsSearchIndex* index_;
        std::string key_;
        SettingScope scope_;
    };

    SettingsSearchIndex() = default;
    ~SettingsSearchIndex() = default;
    SettingsSearchIndex(SettingsSearchIndex const&) = delete;
    SettingsSearchIndex(SettingsSearchIndex&&) = delete;
    SettingsSearchIndex& operator=(SettingsSearchIndex const&) = delete;
    SettingsSearchIndex& operator=(SettingsSearchIndex&&) = delete;

    /**
     * @brief Searches the rendered settings in the current language. Tags are also searched in English.
     *
     * @param query The search input as typed.
     * @param maximumResults The most results to return.
     */
    std::vector<SearchResult> search(std::string_view query, std::size_t maximumResults);

    /**
     * @brief The entries in registration order.
     */
    std::vector<Entry const*> entries() const;

    /**
     * @brief Problems that would make results dangle or mislead: settings constructed but never rendered and help
     * texts missing in English. The caller also checks that every html id resolves to exactly one element.
     */
    std::vector<std::string> verify() const;

  private:
    void add(SettingIdentity const& identity, std::string const& key);
    void remove(std::string const& key, SettingScope scope);
    void markRendered(std::string const& key, SettingScope scope);
    void rebuildCandidatesIfOutdated();

  private:
    std::unordered_map<std::string, Entry> entries_{};
    std::vector<std::string> order_{};
    std::size_t generation_{0u};

    std::vector<Entry const*> candidateEntries_{};
    std::vector<Utility::FuzzySearch::SearchCandidate> candidates_{};
    std::vector<std::vector<std::string>> candidateTags_{};
    std::string candidatesLanguage_{};
    std::optional<std::size_t> candidatesGeneration_{};
};
