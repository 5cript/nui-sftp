#include <frontend/settings/search/settings_search_index.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <utility>

namespace
{
    std::string entryKey(SettingKeyPath const& path)
    {
        return fmt::format("{}", fmt::join(path, "/"));
    }

    std::vector<std::string> splitTags(std::optional<std::string> const& tags)
    {
        std::vector<std::string> result;
        if (!tags)
            return result;

        std::string current;
        const auto flush = [&]()
        {
            const auto first = current.find_first_not_of(' ');
            if (first != std::string::npos)
                result.push_back(current.substr(first, current.find_last_not_of(' ') - first + 1));
            current.clear();
        };
        for (const auto character : *tags)
        {
            if (character == ',')
                flush();
            else
                current.push_back(character);
        }
        flush();
        return result;
    }
}

bool SettingsSearchIndex::Entry::isRendered() const
{
    return std::any_of(
        scopes.begin(),
        scopes.end(),
        [](auto const& scope)
        {
            return scope.second.rendered;
        }
    );
}

SettingsSearchIndex::ScopeRegistration const* SettingsSearchIndex::Entry::renderedScope(SettingScope scope) const
{
    const auto iterator = scopes.find(scope);
    if (iterator == scopes.end() || !iterator->second.rendered)
        return nullptr;
    return &iterator->second;
}

SettingsSearchIndex::Registration::Registration(SettingIdentity const& identity)
    : index_{identity.index()}
    , key_{entryKey(identity.path())}
    , scope_{identity.scope()}
{
    if (index_)
        index_->add(identity, key_);
}

SettingsSearchIndex::Registration::~Registration()
{
    if (index_)
        index_->remove(key_, scope_);
}

void SettingsSearchIndex::Registration::markRendered()
{
    if (index_)
        index_->markRendered(key_, scope_);
}

void SettingsSearchIndex::add(SettingIdentity const& identity, std::string const& key)
{
    auto [iterator, inserted] = entries_.try_emplace(key);
    auto& entry = iterator->second;
    if (inserted)
    {
        entry.path = identity.path();
        entry.labelKey = identity.labelKey();
        entry.helpTextKey = identity.helpTextKey();
        entry.tagsKey = identity.tagsKey();
        order_.push_back(key);
    }
    else if (entry.labelKey != identity.labelKey() || entry.helpTextKey != identity.helpTextKey())
    {
        Log::error("Settings search: setting '{}' is registered with differing language keys.", key);
    }

    const auto [scopeIterator, scopeInserted] = entry.scopes.try_emplace(
        identity.scope(),
        ScopeRegistration{
            .htmlId = identity.htmlId(),
            .breadcrumb = identity.breadcrumb(),
            .engineType = identity.engineType(),
            .groupKey = identity.groupKey(),
        }
    );
    if (!scopeInserted)
        Log::error(
            "Settings search: setting '{}' is registered twice in scope '{}'.", key, settingScopeName(identity.scope())
        );
    ++generation_;
}

void SettingsSearchIndex::remove(std::string const& key, SettingScope scope)
{
    const auto iterator = entries_.find(key);
    if (iterator == entries_.end())
        return;
    iterator->second.scopes.erase(scope);
    if (iterator->second.scopes.empty())
    {
        entries_.erase(iterator);
        std::erase(order_, key);
    }
    ++generation_;
}

void SettingsSearchIndex::markRendered(std::string const& key, SettingScope scope)
{
    const auto iterator = entries_.find(key);
    if (iterator == entries_.end())
        return;
    const auto scopeIterator = iterator->second.scopes.find(scope);
    if (scopeIterator == iterator->second.scopes.end() || scopeIterator->second.rendered)
        return;
    scopeIterator->second.rendered = true;
    ++generation_;
}

std::vector<SettingsSearchIndex::Entry const*> SettingsSearchIndex::entries() const
{
    std::vector<Entry const*> result;
    result.reserve(order_.size());
    for (auto const& key : order_)
        result.push_back(&entries_.at(key));
    return result;
}

void SettingsSearchIndex::rebuildCandidatesIfOutdated()
{
    auto const& currentLanguage = language->currentLanguage();
    if (candidatesGeneration_ == generation_ && candidatesLanguage_ == currentLanguage)
        return;

    candidateEntries_.clear();
    candidates_.clear();
    candidateTags_.clear();
    for (auto const& key : order_)
    {
        auto const& entry = entries_.at(key);
        if (!entry.isRendered())
            continue;

        auto tags = splitTags(language->findByPath(currentLanguage, entry.tagsKey));
        for (auto& englishTag : splitTags(language->findByPath("en_US", entry.tagsKey)))
        {
            if (std::find(tags.begin(), tags.end(), englishTag) == tags.end())
                tags.push_back(std::move(englishTag));
        }

        Utility::FuzzySearch::SearchCandidate candidate{
            .label = Utility::FuzzySearch::NormalizedText::fromUtf8(language
                    ->findByPathWithFallback(currentLanguage, entry.labelKey)
                    .value_or(entry.path.empty() ? std::string{} : entry.path.back())),
            .helpText = Utility::FuzzySearch::NormalizedText::fromUtf8(
                language->findByPathWithFallback(currentLanguage, entry.helpTextKey).value_or("")
            ),
        };
        for (auto const& tag : tags)
            candidate.tags.push_back(Utility::FuzzySearch::NormalizedText::fromUtf8(tag));

        candidateEntries_.push_back(&entry);
        candidates_.push_back(std::move(candidate));
        candidateTags_.push_back(std::move(tags));
    }
    candidatesLanguage_ = currentLanguage;
    candidatesGeneration_ = generation_;
}

std::vector<SettingsSearchIndex::SearchResult>
SettingsSearchIndex::search(std::string_view query, std::size_t maximumResults)
{
    rebuildCandidatesIfOutdated();

    std::vector<SearchResult> results;
    for (auto const& match : Utility::FuzzySearch::rank(query, candidates_, maximumResults))
    {
        SearchResult result{.entry = candidateEntries_[match.index], .match = match};
        if (match.field == Utility::FuzzySearch::MatchedField::Tag)
            result.matchedTag = candidateTags_[match.index][match.matchedTag];
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<std::string> SettingsSearchIndex::verify() const
{
    std::vector<std::string> problems;
    for (auto const& key : order_)
    {
        auto const& entry = entries_.at(key);
        for (auto const& [scope, registration] : entry.scopes)
        {
            if (!registration.rendered)
                problems.push_back(
                    fmt::format(
                        "setting '{}' is constructed in scope '{}' but never rendered", key, settingScopeName(scope)
                    )
                );
        }
        if (!language->findByPath("en_US", entry.helpTextKey))
            problems.push_back(
                fmt::format("setting '{}' has no English help text '{}'", key, fmt::join(entry.helpTextKey, "/"))
            );
    }
    return problems;
}
