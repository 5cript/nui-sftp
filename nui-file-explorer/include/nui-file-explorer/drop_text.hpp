#pragma once

#include <paths/local_path.hpp>
#include <paths/local_path_segment.hpp>
#include <paths/utf8.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NuiFileExplorer::Detail
{
    /**
     * @brief The longest dropped text taken for a path, the Windows limit for long paths.
     */
    inline constexpr std::size_t maxDroppedPathLength = 32767u;

    /**
     * @brief Decodes the %XX escapes of a file URL path, other text stays as it is. Nothing when the result is not
     *        valid UTF-8 or contains a NUL character.
     */
    inline std::optional<std::string> percentDecoded(std::string_view text)
    {
        const auto hexValue = [](char character) -> int
        {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            return -1;
        };
        std::string decoded;
        decoded.reserve(text.size());
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            if (text[index] == '%' && index + 2 < text.size())
            {
                const auto high = hexValue(text[index + 1]);
                const auto low = hexValue(text[index + 2]);
                if (high >= 0 && low >= 0)
                {
                    decoded.push_back(static_cast<char>(high * 16 + low));
                    index += 2;
                    continue;
                }
            }
            decoded.push_back(text[index]);
        }
        if (decoded.find('\0') != std::string::npos || !Paths::isValidUtf8(decoded))
            return std::nullopt;
        return decoded;
    }

    /**
     * @brief True when @p path is absolute on @p platform, '\\' counting as a separator on Windows.
     */
    inline bool isAbsoluteOn(std::string_view path, Paths::LocalPlatform platform)
    {
        std::string generic{path};
        if (platform == Paths::LocalPlatform::Windows)
            std::ranges::replace(generic, '\\', '/');
        return Paths::Detail::rootLengthOf(generic, platform) > 0;
    }

    /**
     * @brief The local path a file URL names, "file:///x" or "file:/x". An empty authority or "localhost" names this
     *        machine; another host is a UNC path on Windows and no local path elsewhere. On Windows "/C:/x" becomes
     *        "C:/x".
     */
    inline std::optional<std::string> pathOfFileUrl(std::string_view url, Paths::LocalPlatform platform)
    {
        const auto [host, encodedPath] = [url]() -> std::pair<std::string_view, std::string_view>
        {
            constexpr std::string_view withAuthority = "file://";
            constexpr std::string_view withoutAuthority = "file:/";
            if (url.starts_with(withAuthority))
            {
                const auto rest = url.substr(withAuthority.size());
                const auto pathStart = std::min(rest.find('/'), rest.size());
                return {rest.substr(0, pathStart), rest.substr(pathStart)};
            }
            if (url.starts_with(withoutAuthority))
                return {std::string_view{}, url.substr(withoutAuthority.size() - 1)};
            return {std::string_view{}, std::string_view{}};
        }();
        if (encodedPath.empty())
            return std::nullopt;
        auto path = percentDecoded(encodedPath);
        if (!path)
            return std::nullopt;

        if (!host.empty() && host != "localhost")
        {
            if (platform != Paths::LocalPlatform::Windows)
                return std::nullopt;
            const auto decodedHost = percentDecoded(host);
            if (!decodedHost)
                return std::nullopt;
            return fmt::format("//{}{}", *decodedHost, *path);
        }

        const auto isLetter = [](char character)
        {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        };
        const bool hasDrive = path->size() >= 3 && (*path)[0] == '/' && isLetter((*path)[1]) && (*path)[2] == ':' &&
            (path->size() == 3 || (*path)[3] == '/');
        if (platform == Paths::LocalPlatform::Windows && hasDrive)
            path->erase(0, 1);
        return path;
    }

    /**
     * @brief The absolute local path a dropped line names: a file URL or an absolute path of @p platform. Any other
     *        text, like words with a slash in them, is no dropped file.
     */
    inline std::optional<std::string> droppedPathOf(std::string_view text, Paths::LocalPlatform platform)
    {
        if (text.empty() || text.size() > maxDroppedPathLength)
            return std::nullopt;
        const auto path = [text, platform]() -> std::optional<std::string>
        {
            if (text.starts_with("file:"))
                return pathOfFileUrl(text, platform);
            if (text.find('\0') != std::string_view::npos || !Paths::isValidUtf8(text))
                return std::nullopt;
            return std::string{text};
        }();
        if (!path || !isAbsoluteOn(*path, platform))
            return std::nullopt;
        return path;
    }

    /**
     * @brief The non-empty lines of dropped text without surrounding white space, so a path with spaces stays whole.
     */
    inline std::vector<std::string_view> droppedLines(std::string_view text)
    {
        constexpr std::string_view whiteSpace = " \t\r\n\f\v";
        std::vector<std::string_view> lines{};
        while (!text.empty())
        {
            const auto lineEnd = std::min(text.find('\n'), text.size());
            auto line = text.substr(0, lineEnd);
            text.remove_prefix(std::min(lineEnd + 1, text.size()));
            const auto first = line.find_first_not_of(whiteSpace);
            if (first == std::string_view::npos)
                continue;
            line = line.substr(first, line.find_last_not_of(whiteSpace) - first + 1);
            lines.push_back(line);
        }
        return lines;
    }

    /**
     * @brief The file URL that is the text of the last link in @p html, like "<a href=...>file:///home/x</a>".
     *        Nothing when the html does not end in such a link.
     */
    inline std::optional<std::string_view> extractFromHtmlLink(std::string_view html)
    {
        constexpr std::string_view linkEnd = "</a>";
        if (!html.ends_with(linkEnd))
            return std::nullopt;
        html.remove_suffix(linkEnd.size());
        const auto tagEnd = html.rfind('>');
        if (tagEnd == std::string_view::npos)
            return std::nullopt;
        const auto fileUrl = html.substr(tagEnd + 1);
        if (!fileUrl.starts_with("file://"))
            return std::nullopt;
        return fileUrl;
    }
}
