#include <utility/command_template.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>

namespace Utility::CommandTemplate
{
    namespace
    {
        struct Token
        {
            /// Index of the opening brace.
            std::size_t begin;
            /// Index one past the closing brace.
            std::size_t end;
            /// The variable name between the braces.
            std::string name;
        };

        bool isWordCharacter(char const character)
        {
            const auto value = static_cast<unsigned char>(character);
            return std::isalnum(value) != 0 || character == '_';
        }

        bool isSpace(char const character)
        {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        }

        /**
         * @brief Parses a token at the given "{{", or nothing when it is not a well formed token.
         */
        std::optional<Token> parseTokenAt(std::string_view const command, std::size_t const begin)
        {
            auto position = begin + 2;
            while (position < command.size() && isSpace(command[position]))
                ++position;

            const auto nameBegin = position;
            while (position < command.size() && isWordCharacter(command[position]))
                ++position;

            const auto nameEnd = position;
            if (nameEnd == nameBegin)
                return std::nullopt;

            while (position < command.size() && isSpace(command[position]))
                ++position;

            if (command.substr(position, 2) != "}}")
                return std::nullopt;

            return Token{
                .begin = begin,
                .end = position + 2,
                .name = std::string{command.substr(nameBegin, nameEnd - nameBegin)},
            };
        }

        std::vector<Token> parseTokens(std::string_view const command)
        {
            std::vector<Token> tokens{};
            for (std::size_t position = 0; position + 1 < command.size();)
            {
                if (command.substr(position, 2) != "{{")
                {
                    ++position;
                    continue;
                }

                auto token = parseTokenAt(command, position);
                if (!token)
                {
                    // Only skip a single brace, so that "{{{name}}" still finds "{{name}}".
                    ++position;
                    continue;
                }

                position = token->end;
                tokens.push_back(std::move(*token));
            }
            return tokens;
        }
    }

    std::vector<std::string> parseVariables(std::string_view const command)
    {
        const auto tokens = parseTokens(command);

        std::vector<std::string> names{};
        names.reserve(tokens.size());
        for (auto const& token : tokens)
        {
            if (std::ranges::find(names, token.name) == names.end())
                names.push_back(token.name);
        }
        return names;
    }

    std::string
    substitute(std::string_view const command, std::map<std::string, std::string> const& values, bool const keepUnfilled)
    {
        const auto tokens = parseTokens(command);

        std::string result{};
        result.reserve(command.size());

        std::size_t copied = 0;
        for (auto const& token : tokens)
        {
            result.append(command.substr(copied, token.begin - copied));
            copied = token.end;

            const auto value = values.find(token.name);
            if (value != values.end())
                result.append(value->second);
            else if (keepUnfilled)
                result.append(command.substr(token.begin, token.end - token.begin));
        }
        result.append(command.substr(copied));
        return result;
    }
}
