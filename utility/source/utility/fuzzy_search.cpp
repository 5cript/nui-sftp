#include <utility/fuzzy_search.hpp>
#include <utility/utf8.hpp>

#include <rapidfuzz/fuzz.hpp>

#include <algorithm>
#include <optional>

namespace Utility::FuzzySearch
{
    namespace
    {
        constexpr double labelWeight = 1.;
        constexpr double tagWeight = 0.9;
        constexpr double helpTextWeight = 0.6;
        constexpr double labelMinimumScore = 70.;
        constexpr double tagMinimumScore = 70.;
        constexpr double helpTextMinimumScore = 85.;
        constexpr std::size_t shortTokenLength = 4u;
        constexpr double shortTokenMinimumScore = 85.;
        constexpr double labelPrefixBonus = 15.;
        constexpr double labelSubstringBonus = 10.;
        constexpr double perfectScore = 100.;

        bool isEven(char32_t codePoint)
        {
            return codePoint % 2u == 0u;
        }

        char32_t lowerLatinExtendedA(char32_t codePoint)
        {
            if (codePoint == 0x130u)
                return U'i';
            if (codePoint == 0x178u)
                return 0xFFu;
            if ((codePoint >= 0x100u && codePoint <= 0x137u) || (codePoint >= 0x14Au && codePoint <= 0x177u))
                return isEven(codePoint) ? codePoint + 1u : codePoint;
            if ((codePoint >= 0x139u && codePoint <= 0x148u) || (codePoint >= 0x179u && codePoint <= 0x17Eu))
                return isEven(codePoint) ? codePoint : codePoint + 1u;
            return codePoint;
        }

        char32_t lowerGreek(char32_t codePoint)
        {
            if (codePoint >= 0x391u && codePoint <= 0x3ABu && codePoint != 0x3A2u)
                return codePoint + 32u;
            if (codePoint == 0x386u)
                return 0x3ACu;
            if (codePoint >= 0x388u && codePoint <= 0x38Au)
                return codePoint + 37u;
            if (codePoint == 0x38Cu)
                return 0x3CCu;
            if (codePoint == 0x38Eu || codePoint == 0x38Fu)
                return codePoint + 63u;
            if (codePoint >= 0x3D8u && codePoint <= 0x3EFu)
                return isEven(codePoint) ? codePoint + 1u : codePoint;
            return codePoint;
        }

        char32_t lowerCyrillic(char32_t codePoint)
        {
            if (codePoint >= 0x400u && codePoint <= 0x40Fu)
                return codePoint + 80u;
            if (codePoint >= 0x410u && codePoint <= 0x42Fu)
                return codePoint + 32u;
            if ((codePoint >= 0x460u && codePoint <= 0x481u) || (codePoint >= 0x48Au && codePoint <= 0x4BFu) ||
                (codePoint >= 0x4D0u && codePoint <= 0x52Fu))
                return isEven(codePoint) ? codePoint + 1u : codePoint;
            if (codePoint == 0x4C0u)
                return 0x4CFu;
            if (codePoint >= 0x4C1u && codePoint <= 0x4CEu)
                return isEven(codePoint) ? codePoint : codePoint + 1u;
            return codePoint;
        }

        bool isAsciiAlphanumeric(char32_t codePoint)
        {
            return (codePoint >= U'0' && codePoint <= U'9') || (codePoint >= U'a' && codePoint <= U'z') ||
                (codePoint >= U'A' && codePoint <= U'Z');
        }

        /**
         * @brief How well one query token matches a field. Partial ratio is only used against tokens at least as
         * long as the query token, because it would rate a short token like "a" inside "parallel" as perfect.
         */
        double tokenScore(std::u32string const& queryToken, NormalizedText const& field)
        {
            if (field.text.find(queryToken) != std::u32string::npos)
                return perfectScore;

            double best = 0.;
            for (auto const& token : field.tokens)
            {
                const auto score = token.size() >= queryToken.size() ? rapidfuzz::fuzz::partial_ratio(queryToken, token)
                                                                     : rapidfuzz::fuzz::ratio(queryToken, token);
                best = std::max(best, score);
            }
            return best;
        }

        struct FieldScore
        {
            double weighted{0.};
            MatchedField field{MatchedField::Label};
            std::size_t tagIndex{0u};
        };

        std::optional<FieldScore> bestFieldFor(std::u32string const& queryToken, SearchCandidate const& candidate)
        {
            std::optional<FieldScore> best{};
            // Short query tokens get rated high by a single differing letter, so they need closer matches.
            const auto shortQuery = queryToken.size() <= shortTokenLength;
            const auto consider =
                [&best,
                    shortQuery](double score, double minimum, double weight, MatchedField field, std::size_t tagIndex)
            {
                if (score < (shortQuery ? std::max(minimum, shortTokenMinimumScore) : minimum))
                    return;
                const auto weighted = score * weight;
                if (!best || weighted > best->weighted)
                    best = FieldScore{.weighted = weighted, .field = field, .tagIndex = tagIndex};
            };

            consider(tokenScore(queryToken, candidate.label), labelMinimumScore, labelWeight, MatchedField::Label, 0u);
            for (std::size_t tagIndex = 0u; tagIndex < candidate.tags.size(); ++tagIndex)
            {
                consider(
                    tokenScore(queryToken, candidate.tags[tagIndex]),
                    tagMinimumScore,
                    tagWeight,
                    MatchedField::Tag,
                    tagIndex
                );
            }
            // Long help texts offer many near misses for short tokens, so those have to appear literally.
            consider(
                tokenScore(queryToken, candidate.helpText),
                shortQuery ? perfectScore : helpTextMinimumScore,
                helpTextWeight,
                MatchedField::HelpText,
                0u
            );
            return best;
        }

        std::u32string joined(std::vector<std::u32string> const& tokens)
        {
            std::u32string result;
            for (auto const& token : tokens)
            {
                if (!result.empty())
                    result.push_back(U' ');
                result += token;
            }
            return result;
        }

        std::optional<RankedMatch> scoreCandidate(
            std::vector<std::u32string> const& queryTokens,
            std::u32string const& joinedQuery,
            SearchCandidate const& candidate,
            std::size_t index
        )
        {
            double total = 0.;
            bool anyLabel = false;
            std::optional<FieldScore> bestTag{};
            for (auto const& queryToken : queryTokens)
            {
                const auto fieldScore = bestFieldFor(queryToken, candidate);
                if (!fieldScore)
                    return std::nullopt;
                total += fieldScore->weighted;
                anyLabel = anyLabel || fieldScore->field == MatchedField::Label;
                if (fieldScore->field == MatchedField::Tag && (!bestTag || fieldScore->weighted > bestTag->weighted))
                    bestTag = fieldScore;
            }

            auto score = total / static_cast<double>(queryTokens.size());
            if (candidate.label.text.starts_with(joinedQuery))
                score += labelPrefixBonus;
            else if (candidate.label.text.find(joinedQuery) != std::u32string::npos)
                score += labelSubstringBonus;

            if (anyLabel)
                return RankedMatch{.index = index, .score = score, .field = MatchedField::Label};
            if (bestTag)
                return RankedMatch{
                    .index = index, .score = score, .field = MatchedField::Tag, .matchedTag = bestTag->tagIndex
                };
            return RankedMatch{.index = index, .score = score, .field = MatchedField::HelpText};
        }
    }

    char32_t toLowerCase(char32_t codePoint)
    {
        if (codePoint < 0x80u)
            return codePoint >= U'A' && codePoint <= U'Z' ? codePoint + 32u : codePoint;
        if (codePoint >= 0xC0u && codePoint <= 0xDEu && codePoint != 0xD7u)
            return codePoint + 32u;
        if (codePoint >= 0x100u && codePoint <= 0x17Fu)
            return lowerLatinExtendedA(codePoint);
        if (codePoint >= 0x370u && codePoint <= 0x3FFu)
            return lowerGreek(codePoint);
        if (codePoint >= 0x400u && codePoint <= 0x52Fu)
            return lowerCyrillic(codePoint);
        if (codePoint == 0x1E9Eu)
            return 0xDFu;
        if (codePoint >= 0xFF21u && codePoint <= 0xFF3Au)
            return codePoint + 32u;
        return codePoint;
    }

    bool isSeparator(char32_t codePoint)
    {
        if (codePoint < 0x80u)
            return !isAsciiAlphanumeric(codePoint);
        if (codePoint <= 0xBFu)
            return codePoint != 0xAAu && codePoint != 0xB5u && codePoint != 0xBAu;
        if (codePoint == 0xD7u || codePoint == 0xF7u || codePoint == 0x1680u || codePoint == 0xFEFFu)
            return true;
        if (codePoint >= 0x2000u && codePoint <= 0x206Fu)
            return true;
        if (codePoint >= 0x3000u && codePoint <= 0x303Fu)
            return codePoint < 0x3005u || codePoint > 0x3007u;
        return (codePoint >= 0xFF01u && codePoint <= 0xFF0Fu) || (codePoint >= 0xFF1Au && codePoint <= 0xFF20u) ||
            (codePoint >= 0xFF3Bu && codePoint <= 0xFF40u) || (codePoint >= 0xFF5Bu && codePoint <= 0xFF65u);
    }

    bool isHanIdeograph(char32_t codePoint)
    {
        return (codePoint >= 0x3400u && codePoint <= 0x4DBFu) || (codePoint >= 0x4E00u && codePoint <= 0x9FFFu) ||
            (codePoint >= 0xF900u && codePoint <= 0xFAFFu) || (codePoint >= 0x20000u && codePoint <= 0x3134Fu);
    }

    std::u32string normalize(std::string_view utf8)
    {
        auto result = Utf8::decode(utf8);
        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](char32_t codePoint)
            {
                return toLowerCase(codePoint);
            }
        );
        return result;
    }

    std::vector<std::u32string> tokenize(std::u32string_view normalized)
    {
        std::vector<std::u32string> result;
        std::u32string currentToken;
        for (const auto codePoint : normalized)
        {
            if (!isSeparator(codePoint))
            {
                currentToken.push_back(codePoint);
                continue;
            }
            if (!currentToken.empty())
            {
                result.push_back(std::move(currentToken));
                currentToken.clear();
            }
        }
        if (!currentToken.empty())
            result.push_back(std::move(currentToken));
        return result;
    }

    NormalizedText NormalizedText::fromUtf8(std::string_view utf8)
    {
        auto text = normalize(utf8);
        auto tokens = tokenize(text);
        return NormalizedText{.text = std::move(text), .tokens = std::move(tokens)};
    }

    double bestPartialRatio(
        std::vector<std::u32string> const& itemTokens,
        std::vector<std::u32string> const& queryTokens,
        double stopAt
    )
    {
        double best = 0.;
        for (auto const& queryToken : queryTokens)
        {
            for (auto const& itemToken : itemTokens)
            {
                const auto score = rapidfuzz::fuzz::partial_ratio(queryToken, itemToken);
                if (score >= stopAt)
                    return score;
                best = std::max(best, score);
            }
        }
        return best;
    }

    bool isQueryLongEnough(std::u32string_view normalizedQuery)
    {
        std::size_t letters = 0u;
        for (const auto codePoint : normalizedQuery)
        {
            if (isHanIdeograph(codePoint))
                return true;
            if (!isSeparator(codePoint))
                ++letters;
        }
        return letters >= 2u;
    }

    std::vector<RankedMatch>
    rank(std::string_view query, std::span<SearchCandidate const> candidates, std::size_t maximumResults)
    {
        auto queryTokens = tokenize(normalize(query));
        std::erase_if(
            queryTokens,
            [](std::u32string const& token)
            {
                return !isQueryLongEnough(token);
            }
        );
        if (queryTokens.empty() || maximumResults == 0u)
            return {};

        const auto joinedQuery = joined(queryTokens);
        std::vector<RankedMatch> matches;
        for (std::size_t index = 0u; index < candidates.size(); ++index)
        {
            if (auto match = scoreCandidate(queryTokens, joinedQuery, candidates[index], index))
                matches.push_back(*match);
        }

        std::stable_sort(
            matches.begin(),
            matches.end(),
            [](RankedMatch const& lhs, RankedMatch const& rhs)
            {
                return lhs.score > rhs.score;
            }
        );
        if (matches.size() > maximumResults)
            matches.resize(maximumResults);
        return matches;
    }
}
