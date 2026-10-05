#include <frontend/session_components/command_panel_helpers.hpp>

#include <utility/language.hpp>

#include <rapidfuzz/fuzz.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cctype>

namespace CommandPanels
{
    namespace
    {
        constexpr double searchHitScore = 75.;
        constexpr std::int64_t secondsPerDay = 86'400;
    }

    std::string lowercased(std::string_view text)
    {
        std::string result{text};
        std::ranges::transform(result, result.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    }

    bool matchesSearch(std::string const& loweredQuery, std::string const& text)
    {
        if (loweredQuery.empty())
            return true;
        const auto loweredText = lowercased(text);
        if (loweredText.find(loweredQuery) != std::string::npos)
            return true;
        return rapidfuzz::fuzz::partial_ratio(loweredQuery, loweredText) >= searchHitScore;
    }

    std::string relativeTime(std::int64_t epochSeconds, std::int64_t nowEpoch)
    {
        const auto delta = std::max<std::int64_t>(0, nowEpoch - epochSeconds);
        if (delta < 60)
            return std::string{language->get("commandPanels", "justNow")};
        if (delta < 3'600)
            return fmt::format(fmt::runtime(std::string{language->get("commandPanels", "minutesAgo")}), delta / 60);
        if (delta < secondsPerDay)
            return fmt::format(fmt::runtime(std::string{language->get("commandPanels", "hoursAgo")}), delta / 3'600);
        return fmt::format(
            fmt::runtime(std::string{language->get("commandPanels", "daysAgo")}), delta / secondsPerDay
        );
    }

    Nui::ElementRenderer
    highlightedText(std::string const& text, std::string const& loweredQuery, std::string const& cssClass)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;

        if (!loweredQuery.empty())
        {
            const auto position = lowercased(text).find(loweredQuery);
            if (position != std::string::npos)
            {
                return span{class_ = cssClass}(
                    Nui::Elements::text{text.substr(0, position)}(),
                    mark{}(text.substr(position, loweredQuery.size())),
                    Nui::Elements::text{text.substr(position + loweredQuery.size())}()
                );
            }
        }
        return span{class_ = cssClass}(text);
    }

    Nui::Attribute disabledWhileDisconnected(Nui::Observed<bool>& connectionLost)
    {
        return Nui::Attributes::disabled = Nui::observe(connectionLost).generate([&connectionLost]() {
            return connectionLost.value();
        });
    }

    Nui::Attribute connectionTooltip(Nui::Observed<bool>& connectionLost, std::string tooltip)
    {
        return Nui::Attributes::title =
                   Nui::observe(connectionLost).generate([&connectionLost, tooltip = std::move(tooltip)]() {
                       if (connectionLost.value())
                           return std::string{language->get("commandPanels", "disconnectedTooltip")};
                       return tooltip;
                   });
    }
}
