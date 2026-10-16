#pragma once

#include <backend/sftp/operation.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <ssh/sftp_session.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

/**
 * @brief The checks that keep a sync entry from replacing or deleting what changed after its compare. The path is
 *        inspected right before the move or the removal, a change in the moment between the two goes unnoticed.
 */
namespace ComparedStateCheck
{
    /**
     * @brief Whole seconds since the epoch of @p time, how a local scan records modification times.
     */
    std::uint64_t secondsSinceEpoch(std::filesystem::file_time_type time);

    /**
     * @brief What is at the local @p path now, without following a link, as a local scan sees it. A link or junction
     *        on Windows has an unknown type, the scan excludes it.
     */
    std::expected<SharedData::ComparedState, std::error_code> localStateOf(std::filesystem::path const& path);

    /**
     * @brief What is at the remote @p path now, without following a link.
     */
    std::expected<SharedData::ComparedState, SecureShell::SftpError>
    remoteStateOfInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& path);

    /**
     * @brief Fails with ChangedSinceCompare when the local @p path no longer holds @p compared (see
     *        SharedData::unchangedSinceCompare), and with FileStatFailed when it cannot be inspected. Without
     *        @p compared there is nothing to check.
     */
    std::expected<void, Operation::Error>
    requireUnchangedLocal(std::filesystem::path const& path, std::optional<SharedData::ComparedState> const& compared);

    /**
     * @brief The remote counterpart of @ref requireUnchangedLocal, failing with SftpError when the server cannot
     *        tell.
     */
    std::expected<void, Operation::Error> requireUnchangedRemoteInStrand(
        SecureShell::SftpSession& sftp,
        Paths::RemotePath const& path,
        std::optional<SharedData::ComparedState> const& compared
    );

    /**
     * @brief Utility::moveIntoPlace of a finished local transfer, once @ref requireUnchangedLocal passed for @p to.
     */
    std::expected<void, Operation::Error> moveLocalIntoPlace(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        bool replace,
        std::optional<SharedData::ComparedState> const& compared
    );

    /**
     * @brief SftpSession::moveIntoPlaceInStrand of a finished remote transfer, once
     *        @ref requireUnchangedRemoteInStrand passed for @p to.
     */
    std::expected<void, Operation::Error> moveRemoteIntoPlaceInStrand(
        SecureShell::SftpSession& sftp,
        Paths::RemotePath const& from,
        Paths::RemotePath const& to,
        bool replace,
        std::string const& asideSuffix,
        SharedData::FileType sourceType,
        std::optional<SharedData::ComparedState> const& compared
    );
}
