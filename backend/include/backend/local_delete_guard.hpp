#pragma once

#include <paths/local_path.hpp>

#include <fmt/format.h>

#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

/**
 * @brief Refuses a root directory and @p home or a directory containing it.
 */
inline std::expected<void, std::string>
refuseProtectedLocalDelete(Paths::LocalPath const& target, std::optional<Paths::LocalPath> const& home)
{
    if (target.empty())
        return std::unexpected(std::string{"no path"});
    if (target.isRoot())
        return std::unexpected(fmt::format("'{}' is a root directory", target.toUtf8()));
    if (home && home->isWithin(target))
        return std::unexpected(fmt::format("'{}' contains the home directory", target.toUtf8()));
    return {};
}

/**
 * @brief A local path that may be deleted, or the reason to refuse it: not absolute, containing '..', a root
 *        directory, or @p home or a directory containing it.
 */
inline std::expected<Paths::LocalPath, std::string>
localDeleteTarget(std::string const& utf8, std::optional<Paths::LocalPath> const& home)
{
    // LocalPath resolves ".." lexically, which goes elsewhere when a directory on the way is a symlink.
    for (auto const segment : std::views::split(std::string_view{utf8}, '/'))
    {
        for (auto const part : std::views::split(segment, '\\'))
        {
            if (std::string_view{part.begin(), part.end()} == "..")
                return std::unexpected(fmt::format("'{}' contains '..'", utf8));
        }
    }

    auto target = Paths::LocalPath::fromUtf8(utf8);
    if (!target)
        return std::unexpected(fmt::format("'{}' {}", utf8, Paths::toString(target.error().reason)));
    if (auto allowed = refuseProtectedLocalDelete(*target, home); !allowed)
        return std::unexpected(std::move(allowed).error());
    return *std::move(target);
}

/**
 * @brief The home directory of the user running the backend, empty when it cannot be determined.
 */
std::optional<Paths::LocalPath> localHomeDirectory();
