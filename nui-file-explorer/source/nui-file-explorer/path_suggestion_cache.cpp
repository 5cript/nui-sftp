#include <nui-file-explorer/path_suggestion_cache.hpp>
#include <rapidfuzz/fuzz.hpp>

#include <iterator>
#include <algorithm>

namespace NuiFileExplorer
{
    PathSuggestionCache::PathSuggestionCache(GeneratorT&& generator, double fuzzScoreCutOff)
        : generator_{std::move(generator)}
        , fuzzScoreCutOff_{fuzzScoreCutOff}
    {}

    void PathSuggestionCache::generateSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    )
    {
        const auto baseDirectory = path.parent_path();

        auto cachedSuggestions = lookupCache(baseDirectory);
        if (cachedSuggestions)
        {
            return onResultsAvailable(fuzzSuggestions(*cachedSuggestions, path.filename().string(), maxSuggestions));
        }

        // Populate Cache:
        generator_(
            baseDirectory,
            [this, baseDirectory, path, maxSuggestions, onResultsAvailable](
                std::vector<std::filesystem::path> const& suggestions
            )
            {
                cache_[baseDirectory] = suggestions;
                cacheOrder_.push_back(baseDirectory);
                if (cacheOrder_.size() > static_cast<std::size_t>(maxCacheSize))
                {
                    const auto dirToRemove = cacheOrder_.front();
                    cacheOrder_.pop_front();
                    cache_.erase(dirToRemove);
                }
                onResultsAvailable(fuzzSuggestions(suggestions, path.filename().string(), maxSuggestions));
            }
        );
    }

    std::vector<std::filesystem::path> PathSuggestionCache::fuzzSuggestions(
        std::vector<std::filesystem::path> const& suggestions,
        std::string const& filenamePart,
        int maxSuggestions
    ) const
    {
        if (filenamePart.empty())
        {
            if (maxSuggestions <= 0)
                return suggestions;
            const auto count = std::min(static_cast<std::size_t>(maxSuggestions), suggestions.size());
            return {suggestions.begin(), suggestions.begin() + static_cast<std::ptrdiff_t>(count)};
        }

        std::vector<std::pair<std::filesystem::path, double>> ratedResults;
        for (const auto& suggestion : suggestions)
        {
            const auto suggestionStr = suggestion.filename().string();
            const auto score = rapidfuzz::fuzz::partial_ratio(filenamePart, suggestionStr);
            if (score < fuzzScoreCutOff_)
                continue;
            ratedResults.emplace_back(suggestion, score);
        }
        if (ratedResults.empty())
            return {};

        std::sort(
            ratedResults.begin(),
            ratedResults.end(),
            [](auto const& a, auto const& b)
            {
                return a.second > b.second;
            }
        );

        const auto takeCount = (maxSuggestions <= 0)
            ? ratedResults.size()
            : std::min(static_cast<std::size_t>(maxSuggestions), ratedResults.size());

        std::vector<std::filesystem::path> results;
        results.reserve(takeCount);
        std::transform(
            make_move_iterator(ratedResults.begin()),
            make_move_iterator(ratedResults.begin() + static_cast<std::ptrdiff_t>(takeCount)),
            std::back_inserter(results),
            [](auto&& pair)
            {
                return std::move(pair).first;
            }
        );
        return results;
    }

    std::optional<std::vector<std::filesystem::path>>
    PathSuggestionCache::lookupCache(std::filesystem::path const& dirPath)
    {
        auto it = cache_.find(dirPath);
        if (it == cache_.end())
            return std::nullopt;
        return it->second;
    }
}