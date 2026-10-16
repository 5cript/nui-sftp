#pragma once

#include <utility/algorithm/case_convert.hpp>

#include <string>
#include <vector>

namespace Utility
{
    struct SegmentedString
    {
        std::vector<std::string> normalizedSegments;

        std::string camelCase() const
        {
            if (normalizedSegments.empty())
                return {};

            std::string result = normalizedSegments[0];
            for (size_t i = 1; i < normalizedSegments.size(); ++i)
            {
                if (!normalizedSegments[i].empty())
                {
                    result += Algorithm::asciiToUpper(normalizedSegments[i][0]);
                    result += normalizedSegments[i].substr(1);
                }
            }
            return result;
        }

        std::string pascalCase() const
        {
            if (normalizedSegments.empty())
                return {};

            std::string result;
            for (const auto& segment : normalizedSegments)
            {
                if (!segment.empty())
                {
                    result += Algorithm::asciiToUpper(segment[0]);
                    result += segment.substr(1);
                }
            }
            return result;
        }

        std::string snakeCase() const
        {
            if (normalizedSegments.empty())
                return {};

            std::string result = normalizedSegments[0];
            for (size_t i = 1; i < normalizedSegments.size(); ++i)
            {
                result += '_';
                result += normalizedSegments[i];
            }
            return result;
        }

        std::string screamingSnakeCase() const
        {
            if (normalizedSegments.empty())
                return {};

            std::string result = normalizedSegments[0];
            for (size_t i = 1; i < normalizedSegments.size(); ++i)
            {
                result += '_';
                for (auto const& ch : normalizedSegments[i])
                    result += Algorithm::asciiToUpper(ch);
            }
            return result;
        }

        std::string kebabCase() const
        {
            if (normalizedSegments.empty())
                return {};

            std::string result = normalizedSegments[0];
            for (size_t i = 1; i < normalizedSegments.size(); ++i)
            {
                result += '-';
                result += normalizedSegments[i];
            }
            return result;
        }

        std::string joined(const std::string& delimiter) const
        {
            if (normalizedSegments.empty())
                return {};

            std::string result = normalizedSegments[0];
            for (size_t i = 1; i < normalizedSegments.size(); ++i)
            {
                result += delimiter;
                result += normalizedSegments[i];
            }
            return result;
        }
    };

    inline SegmentedString splitByCamelCase(std::string const& input)
    {
        SegmentedString result;
        if (input.empty())
            return result;

        std::string currentSegment{};
        for (auto const& ch : input)
        {
            if (Algorithm::isAsciiUpper(ch) && !currentSegment.empty())
            {
                result.normalizedSegments.push_back(currentSegment);
                currentSegment.clear();
            }
            currentSegment += Algorithm::asciiToLower(ch);
        }
        if (!currentSegment.empty())
            result.normalizedSegments.push_back(currentSegment);
        return result;
    }

    inline SegmentedString splitByPascalCase(std::string const& input)
    {
        SegmentedString result;
        if (input.empty())
            return result;

        std::string currentSegment{};
        for (auto const& ch : input)
        {
            if (Algorithm::isAsciiUpper(ch) && !currentSegment.empty())
            {
                result.normalizedSegments.push_back(currentSegment);
                currentSegment.clear();
            }
            currentSegment += Algorithm::asciiToLower(ch);
        }
        if (!currentSegment.empty())
            result.normalizedSegments.push_back(currentSegment);
        return result;
    }

    inline SegmentedString splitBySnakeCase(std::string const& input)
    {
        SegmentedString result;
        if (input.empty())
            return result;

        std::string currentSegment{};
        for (auto const& ch : input)
        {
            if (ch == '_')
            {
                result.normalizedSegments.push_back(currentSegment);
                currentSegment.clear();
            }
            else
            {
                currentSegment += Algorithm::asciiToLower(ch);
            }
        }
        if (!currentSegment.empty())
            result.normalizedSegments.push_back(currentSegment);
        return result;
    }

    inline SegmentedString splitByKebabCase(std::string const& input)
    {
        SegmentedString result;
        if (input.empty())
            return result;

        std::string currentSegment{};
        for (auto const& ch : input)
        {
            if (ch == '-')
            {
                result.normalizedSegments.push_back(currentSegment);
                currentSegment.clear();
            }
            else
            {
                currentSegment += Algorithm::asciiToLower(ch);
            }
        }
        if (!currentSegment.empty())
            result.normalizedSegments.push_back(currentSegment);
        return result;
    }
}