#pragma once

#include <paths/local_path.hpp>

#include <expected>
#include <string>
#include <vector>

/**
 * @brief Refuses an unset path, a root directory, and each of @p homes or a directory containing one.
 *
 * Paths are compared as text, and without ASCII case for Windows paths and on macOS, whose filesystems fold case.
 * A target that is an existing directory (not a link to one) is also compared by file identity with every home and
 * each of its ancestors, which catches other spellings of the same directory: case beyond ASCII, 8.3 names, NFD
 * names and ancestors reached through a symlink.
 */
std::expected<void, std::string>
refuseProtectedLocalDelete(Paths::LocalPath const& target, std::vector<Paths::LocalPath> const& homes);

/**
 * @brief A local path that may be deleted, or the reason to refuse it: not a valid absolute path, containing '..'
 *        (also as '\\..' on Windows) or refused by @ref refuseProtectedLocalDelete.
 */
std::expected<Paths::LocalPath, std::string>
localDeleteTarget(std::string const& utf8, std::vector<Paths::LocalPath> const& homes);

/**
 * @brief The home directories of the user running the backend, which the delete guards protect, each once.
 *
 * On Windows the profile folder (FOLDERID_Profile) and the USERPROFILE and HOME variables, elsewhere the HOME
 * variable and the home of the passwd entry. When none can be determined a warning is logged and only roots stay
 * protected. Never throws.
 */
std::vector<Paths::LocalPath> localHomeDirectories();
