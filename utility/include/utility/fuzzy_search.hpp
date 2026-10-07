#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Utility::FuzzySearch
{
    /**
     * @brief Simple lowercase mapping for ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic and fullwidth Latin.
     * Code points of other scripts, which mostly have no case, come back unchanged.
     */
    char32_t toLowerCase(char32_t codePoint);

    /**
     * @brief Whether the code point separates words: whitespace, control characters and punctuation, including
     * general, CJK and fullwidth punctuation.
     */
    bool isSeparator(char32_t codePoint);

    /**
     * @brief Whether the code point is a Han ideograph, which carries meaning on its own.
     */
    bool isHanIdeograph(char32_t codePoint);

    /**
     * @brief Decodes UTF-8 and lowercases it, the form all matching works on.
     */
    std::u32string normalize(std::string_view utf8);

    /**
     * @brief Splits normalized text at separators. Scripts written without spaces stay one token per run.
     */
    std::vector<std::u32string> tokenize(std::u32string_view normalized);

    /**
     * @brief Text prepared once for repeated matching.
     */
    struct NormalizedText
    {
        std::u32string text{};
        std::vector<std::u32string> tokens{};

        /**
         * @brief Normalizes and tokenizes UTF-8 text.
         */
        static NormalizedText fromUtf8(std::string_view utf8);
    };

    /**
     * @brief The highest partial ratio over all pairs of query and item tokens, stopping early once @p stopAt is
     * reached. Any single query token matching counts.
     */
    double bestPartialRatio(
        std::vector<std::u32string> const& itemTokens,
        std::vector<std::u32string> const& queryTokens,
        double stopAt
    );

    /**
     * @brief Whether a normalized query is long enough to search with: two letters, or one Han ideograph.
     */
    bool isQueryLongEnough(std::u32string_view normalizedQuery);

    /**
     * @brief Something that can be found: a label, a help text and synonym tags.
     */
    struct SearchCandidate
    {
        NormalizedText label{};
        NormalizedText helpText{};
        std::vector<NormalizedText> tags{};
    };

    /**
     * @brief The field that decided a match, most significant first.
     */
    enum class MatchedField
    {
        Label,
        Tag,
        HelpText
    };

    /**
     * @brief A candidate that matched the query.
     */
    struct RankedMatch
    {
        /**
         * @brief Position of the candidate in the searched span.
         */
        std::size_t index{0u};
        double score{0.};
        MatchedField field{MatchedField::Label};
        /**
         * @brief Position of the best matching tag; only meaningful when field is Tag.
         */
        std::size_t matchedTag{0u};
    };

    /**
     * @brief Ranks the candidates against the query. Every query token has to match the label, a tag or the help
     * text. Label matches weigh more than tag matches, which weigh more than help text matches. Equal scores keep
     * the order of the candidates.
     *
     * @param query The search input as typed, in UTF-8.
     * @param candidates The candidates to search.
     * @param maximumResults The most results to return.
     */
    std::vector<RankedMatch>
    rank(std::string_view query, std::span<SearchCandidate const> candidates, std::size_t maximumResults);
}
