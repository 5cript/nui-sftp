#pragma once

#include <ssh/sftp_session.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <vector>

/**
 * @brief Whether each of @p paths exists on the local disk. Only "not found" is absent, a dangling symlink exists. A
 *        path that cannot be checked fails the whole check: a transfer overwrites what it takes for absent.
 */
std::expected<std::vector<bool>, std::string> localPathsExist(std::vector<std::string> const& paths);

/**
 * @brief How many queried children of one remote directory are checked by listing it instead of an lstat each.
 */
constexpr std::size_t remoteListingThreshold = 10;

/**
 * @brief Whether each of @p paths exists on the server, with the same rules as localPathsExist. The children of a
 *        directory with at least @ref remoteListingThreshold of them queried are checked with one listing of it
 *        (names only), the others with one lstat each. A directory that cannot be listed for its permissions has its
 *        children looked up one by one, and so has a name only a case or normalization variant of is listed.
 */
std::expected<std::vector<bool>, std::string> remotePathsExist(
    SecureShell::SftpSession& session,
    std::vector<std::string> const& paths,
    std::chrono::milliseconds timeout
);
