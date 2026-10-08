#pragma once

#include <command-store/snippet_transfer.hpp>
#include <command-store/types.hpp>
#include <utility/expected.hpp>

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Ready-made snippet folders shipped with the application.
 *
 * A preset is a transfer document with a "preset" object naming the folder:
 * {"version": 1, "preset": {"name", "icon", "description": {"en_US": ...}}, "snippets": [...]}.
 */
namespace CommandStore::SnippetPresets
{
    struct Preset
    {
        /** @brief The folder name, also the name the preset is shown with. */
        std::string name{};

        /** @brief Name of the icon shown on the preset card. */
        std::string icon{};

        /** @brief Descriptions by language key, like "en_US". */
        std::map<std::string, std::string> descriptions{};

        /** @brief The snippets, each with the preset name as its folder. */
        std::vector<TransferSnippet> snippets{};

        /**
         * @brief The description in the given language, falling back to English.
         */
        std::string description(std::string const& languageKey) const
        {
            if (const auto localized = descriptions.find(languageKey); localized != descriptions.end())
                return localized->second;
            if (const auto english = descriptions.find("en_US"); english != descriptions.end())
                return english->second;
            return {};
        }
    };

    using ParseResult = Utility::Expected<Preset, SnippetTransfer::ParseFailure>;

    /**
     * @brief Reads a preset document; a preset without name or snippets is rejected.
     */
    inline ParseResult parse(std::string_view text)
    {
        using SnippetTransfer::ParseError;
        using SnippetTransfer::ParseFailure;

        const auto document = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (document.is_discarded())
            return Utility::Unexpected{ParseFailure{.error = ParseError::InvalidJson}};
        if (!document.is_object() || !document.contains("preset") || !document["preset"].is_object())
            return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};

        auto const& header = document["preset"];
        Preset preset{
            .name = SnippetTransfer::Detail::trimmed(SnippetTransfer::Detail::stringOr(header, "name")),
            .icon = SnippetTransfer::Detail::stringOr(header, "icon"),
        };
        if (preset.name.empty())
            return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};
        if (const auto descriptions = header.find("description");
            descriptions != header.end() && descriptions->is_object())
        {
            for (auto const& [languageKey, description] : descriptions->items())
            {
                if (description.is_string())
                    preset.descriptions.emplace(languageKey, description.get<std::string>());
            }
        }

        return SnippetTransfer::fromJson(document).and_then(
            [&preset](std::vector<TransferSnippet> snippets) -> ParseResult
            {
                if (snippets.empty())
                    return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};
                for (auto& snippet : snippets)
                    snippet.folder = preset.name;
                preset.snippets = std::move(snippets);
                return std::move(preset);
            }
        );
    }
}
