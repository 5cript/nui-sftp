#include <utility/user_directories.hpp>
#include <utility/path_utf.hpp>

#include <optional>
#include <ranges>

namespace Utility
{
    namespace
    {
        constexpr std::string_view whitespace = " \t\r";

        std::string_view trim(std::string_view text)
        {
            const auto first = text.find_first_not_of(whitespace);
            if (first == std::string_view::npos)
                return {};
            const auto last = text.find_last_not_of(whitespace);
            return text.substr(first, last - first + 1);
        }

        std::optional<std::string> unquote(std::string_view value)
        {
            if (value.size() < 2 || value.front() != '"' || value.back() != '"')
                return std::nullopt;

            std::string result;
            const auto inner = value.substr(1, value.size() - 2);
            for (std::size_t index = 0; index < inner.size(); ++index)
            {
                if (inner[index] == '\\' && index + 1 < inner.size())
                    ++index;
                result.push_back(inner[index]);
            }
            return result;
        }

        std::optional<std::string> expandHome(std::string const& value, std::string_view home)
        {
            constexpr std::string_view homeVariable = "$HOME";
            if (value.starts_with(homeVariable))
            {
                const auto rest = std::string_view{value}.substr(homeVariable.size());
                if (rest.empty() || rest == "/" || !rest.starts_with('/'))
                    return std::nullopt;
                return std::string{home} + std::string{rest};
            }
            if (value.starts_with('/'))
                return value;
            return std::nullopt;
        }
    }

    std::vector<UserDirectory> parseUserDirectories(std::string_view content, std::filesystem::path const& home)
    {
        auto homeString = pathToUtf8Generic(home);
        while (homeString.size() > 1 && homeString.ends_with('/'))
            homeString.pop_back();
        const std::string_view homePrefix = homeString == "/" ? std::string_view{} : std::string_view{homeString};

        constexpr std::string_view prefix = "XDG_";
        constexpr std::string_view suffix = "_DIR";

        std::vector<UserDirectory> directories;
        for (auto const rawLine : content | std::views::split('\n'))
        {
            const auto line = trim(std::string_view{rawLine.begin(), rawLine.end()});
            if (line.empty() || line.starts_with('#'))
                continue;

            const auto equals = line.find('=');
            if (equals == std::string_view::npos)
                continue;

            const auto key = trim(line.substr(0, equals));
            if (!key.starts_with(prefix) || !key.ends_with(suffix) || key.size() <= prefix.size() + suffix.size())
                continue;

            const auto value = unquote(trim(line.substr(equals + 1)));
            if (!value)
                continue;

            auto expanded = expandHome(*value, homePrefix);
            if (!expanded)
                continue;
            while (expanded->size() > 1 && expanded->ends_with('/'))
                expanded->pop_back();
            if (*expanded == homeString)
                continue;

            directories.push_back({
                .name = std::string{key.substr(prefix.size(), key.size() - prefix.size() - suffix.size())},
                .path = pathFromUtf8(*expanded),
            });
        }
        return directories;
    }
}
