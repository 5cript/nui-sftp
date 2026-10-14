#include <shared_data/ignore_rules.hpp>

#include <algorithm>
#include <cstddef>

namespace SharedData
{
    namespace
    {
        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
                text.remove_suffix(1);
            return text;
        }

        /** @brief True if pattern contains a '/' that is not the final char. */
        bool patternIsAnchored(std::string_view pattern)
        {
            if (pattern.empty())
                return false;
            const auto slashPos = pattern.find('/');
            if (slashPos == std::string_view::npos)
                return false;
            return slashPos != pattern.size() - 1;
        }
    }

    bool globMatch(std::string_view pattern, std::string_view text)
    {
        if (pattern.empty())
            return text.empty();

        if (pattern[0] == '*')
        {
            if (pattern.size() > 1 && pattern[1] == '*')
            {
                auto rest = pattern.substr(2);
                if (!rest.empty() && rest[0] == '/')
                    rest.remove_prefix(1);
                for (std::size_t offset = 0; offset <= text.size(); ++offset)
                {
                    if (globMatch(rest, text.substr(offset)))
                        return true;
                }
                return false;
            }
            auto rest = pattern.substr(1);
            for (std::size_t offset = 0; offset <= text.size(); ++offset)
            {
                if (offset > 0 && text[offset - 1] == '/')
                    break;
                if (globMatch(rest, text.substr(offset)))
                    return true;
            }
            return false;
        }

        if (text.empty())
            return false;

        if (pattern[0] == '?')
        {
            if (text[0] == '/')
                return false;
            return globMatch(pattern.substr(1), text.substr(1));
        }

        if (pattern[0] == '\\' && pattern.size() > 1)
        {
            if (text[0] != pattern[1])
                return false;
            return globMatch(pattern.substr(2), text.substr(1));
        }

        if (pattern[0] != text[0])
            return false;
        return globMatch(pattern.substr(1), text.substr(1));
    }

    std::vector<IgnorePattern> parseIgnoreFile(std::string_view content)
    {
        std::vector<IgnorePattern> patterns;
        std::size_t cursor = 0;
        while (cursor <= content.size())
        {
            const auto nextLine = content.find('\n', cursor);
            auto rawLine =
                content.substr(cursor, nextLine == std::string_view::npos ? std::string_view::npos : nextLine - cursor);
            cursor = (nextLine == std::string_view::npos) ? content.size() + 1 : nextLine + 1;

            auto line = trim(rawLine);
            if (line.empty() || line.front() == '#')
                continue;

            IgnorePattern parsed{};
            if (line.front() == '!')
            {
                parsed.negated = true;
                line.remove_prefix(1);
                line = trim(line);
            }
            if (!line.empty() && line.back() == '/')
            {
                parsed.directoryOnly = true;
                line.remove_suffix(1);
            }
            if (!line.empty() && line.front() == '/')
                line.remove_prefix(1);
            if (line.empty())
                continue;

            parsed.anchored = patternIsAnchored(line);
            parsed.pattern = std::string{line};
            patterns.push_back(std::move(parsed));
        }
        return patterns;
    }

    void IgnoreMatcher::addFile(Paths::RelativePath const& relDir, std::string_view content)
    {
        auto patterns = parseIgnoreFile(content);
        if (patterns.empty())
            return;
        ruleSets_.push_back(IgnoreRuleSet{.originDir = relDir, .patterns = std::move(patterns)});
    }

    bool IgnoreMatcher::isIgnored(Paths::RelativePath const& relPath, bool isDirectory) const
    {
        bool ignored = false;
        for (auto const& ruleSet : ruleSets_)
        {
            if (relPath == ruleSet.originDir || !relPath.isWithin(ruleSet.originDir))
                continue;

            const auto relToOriginStr = ruleSet.originDir.empty()
                ? std::string_view{relPath.value()}
                : std::string_view{relPath.value()}.substr(ruleSet.originDir.value().size() + 1u);

            for (auto const& pattern : ruleSet.patterns)
            {
                if (pattern.directoryOnly && !isDirectory)
                    continue;

                bool matched = false;
                if (pattern.anchored)
                {
                    matched = globMatch(pattern.pattern, relToOriginStr);
                }
                else
                {
                    matched = std::ranges::any_of(
                        Paths::Detail::splitAtSlashes(relToOriginStr),
                        [&pattern](std::string_view component)
                        {
                            return globMatch(pattern.pattern, component);
                        }
                    );
                    if (!matched)
                        matched = globMatch(pattern.pattern, relToOriginStr);
                }

                if (matched)
                    ignored = !pattern.negated;
            }
        }

        return ignored;
    }
}
