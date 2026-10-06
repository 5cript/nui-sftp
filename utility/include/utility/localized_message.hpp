#pragma once

#include <fmt/format.h>

#include <string>
#include <string_view>
#include <vector>

namespace Utility
{
    /**
     * @brief Separates the key of a localized message from its arguments. Arguments are paths or system error texts,
     * which never contain the ASCII unit separator.
     */
    constexpr char localizedMessageSeparator = '\x1F';

    /**
     * @brief Builds a message the frontend translates, for code that has no access to the language files.
     *
     * @param key Dot separated path below the language file root, starting with backend.
     * @param arguments Values for the placeholders of the translation, inserted untranslated.
     * @return The key followed by the formatted arguments, each after a separator.
     */
    template <typename... Args>
    std::string localizedMessage(std::string_view key, Args const&... arguments)
    {
        std::string message{key};
        ((message += localizedMessageSeparator, message += fmt::format("{}", arguments)), ...);
        return message;
    }

    struct ParsedLocalizedMessage
    {
        std::string key;
        std::vector<std::string> arguments;
    };

    /**
     * @brief Splits a message built by localizedMessage into its key and arguments. Any other text comes back as the
     * key without arguments.
     */
    inline ParsedLocalizedMessage parseLocalizedMessage(std::string_view message)
    {
        ParsedLocalizedMessage parsed{};
        auto separator = message.find(localizedMessageSeparator);
        parsed.key = std::string{message.substr(0, separator)};
        while (separator != std::string_view::npos)
        {
            const auto start = separator + 1;
            separator = message.find(localizedMessageSeparator, start);
            parsed.arguments.emplace_back(message.substr(start, separator - start));
        }
        return parsed;
    }
}
