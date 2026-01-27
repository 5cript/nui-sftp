#pragma once

#include <filesystem>
#include <vector>
#include <deque>
#include <functional>

namespace NuiFileExplorer
{
    class PathSuggestionCache
    {
      public:
        using GeneratorCallbackT = std::function<void(std::vector<std::filesystem::path> const&)>;
        using GeneratorT = std::function<void(std::filesystem::path const&, GeneratorCallbackT)>;
        constexpr static int maxCacheSize = 20;

        PathSuggestionCache(GeneratorT&& generator, double fuzzScoreCutOff = 50.);
        ~PathSuggestionCache() = default;
        PathSuggestionCache(const PathSuggestionCache&) = delete;
        PathSuggestionCache& operator=(const PathSuggestionCache&) = delete;
        PathSuggestionCache(PathSuggestionCache&&) = default;
        PathSuggestionCache& operator=(PathSuggestionCache&&) = default;

        void generateSuggestions(
            std::filesystem::path const& path,
            int maxSuggestions,
            std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
        );

      private:
        std::optional<std::vector<std::filesystem::path>> lookupCache(std::filesystem::path const& dirPath);
        std::vector<std::filesystem::path> fuzzSuggestions(
            std::vector<std::filesystem::path> const& suggestions,
            std::string const& filenamePart,
            int maxSuggestions
        ) const;

      private:
        // Generates more listings
        GeneratorT generator_;

        // Score at which the results are no longer considered matches.
        double fuzzScoreCutOff_;

        // Map from directory path to list of suggestions within that directory.
        std::unordered_map<std::filesystem::path, std::vector<std::filesystem::path>> cache_{};

        // Circle buffer with paths inside, when the circle buffer reaches maxCacheSize, remove the path from the cache
        // that gets removed from the circle buffer.
        std::deque<std::filesystem::path> cacheOrder_{};
    };
}