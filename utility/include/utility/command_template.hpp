#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Utility::CommandTemplate
{
    /**
     * @brief Extracts the distinct {{variable}} names of a command template.
     *
     * A token is "{{", optional whitespace, a name of word characters ([A-Za-z0-9_]), optional
     * whitespace and "}}". Anything that does not match is plain text. Names are returned in order
     * of first appearance, without duplicates.
     *
     * @param command The command template.
     * @return The variable names, without braces or surrounding whitespace.
     */
    std::vector<std::string> parseVariables(std::string_view command);

    /**
     * @brief Finds the first variable brace that has no counterpart.
     *
     * Reported are a "{{" that is never closed by a "}}" before the next "{{" or the end, and a
     * half written variable like "{name}}". A closed "{{...}}" that is no variable (Go templates such
     * as "{{.State}}") and stray "}}" elsewhere (JSON, awk) are plain text and not reported.
     *
     * @param command The command template.
     * @return The index of the offending brace, or nothing when all variable braces match.
     */
    std::optional<std::size_t> findUnmatchedBrace(std::string_view command);

    /**
     * @brief Replaces every {{variable}} token of a command template with its value.
     *
     * @param command The command template.
     * @param values Values by variable name; substitution is literal, values are not rescanned.
     * @param keepUnfilled When true, tokens without a value are kept verbatim, otherwise they are
     *                     replaced by an empty string.
     * @return The substituted command.
     */
    std::string substitute(
        std::string_view command,
        std::map<std::string, std::string> const& values,
        bool keepUnfilled = true
    );
}
