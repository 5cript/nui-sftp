#pragma once

#include <paths/local_path.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

/**
 * @brief What @ref removeLocalItems removed.
 */
struct LocalRemoval
{
    std::uintmax_t removed{0};
    /** @brief "path: reason" for every item that could not be removed, by the item's own path. */
    std::vector<std::string> failures{};
};

/**
 * @brief A path that @ref removeLocalItems refused, so it removed nothing.
 */
struct LocalRemovalRefusal
{
    std::string path;
    std::string reason;
};

/**
 * @brief Removes each of the local items @p pathsUtf8 by itself, so a directory only when it is empty, without
 *        following links or junctions. An item that cannot be removed does not stop the others. Nothing is removed
 *        when one of the paths is refused by localDeleteTarget. A directory with its content goes through the
 *        LocalOperationQueue instead.
 */
std::expected<LocalRemoval, LocalRemovalRefusal>
removeLocalItems(std::span<std::string const> pathsUtf8, std::vector<Paths::LocalPath> const& homes);
