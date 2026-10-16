#pragma once

#include <ssh/sftp_session.hpp>

#include <chrono>
#include <expected>
#include <string>
#include <vector>

/**
 * @brief Whether each of @p paths exists on the local disk. Only "not found" is absent, a dangling symlink exists. A
 *        path that cannot be checked fails the whole check: a transfer overwrites what it takes for absent.
 */
std::expected<std::vector<bool>, std::string> localPathsExist(std::vector<std::string> const& paths);

/**
 * @brief Whether each of @p paths exists on the server, with the same rules as localPathsExist. Few paths are
 *        checked with one lstat each, more with one listing per parent directory.
 */
std::expected<std::vector<bool>, std::string> remotePathsExist(
    SecureShell::SftpSession& session,
    std::vector<std::string> const& paths,
    std::chrono::milliseconds timeout
);
