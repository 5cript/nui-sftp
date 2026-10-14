#pragma once

#include <paths/remote_path.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace SecureShell
{
    class SftpSession;
}

/**
 * @brief Why a remote delete target is protected.
 */
struct RemoteDeleteRefusal
{
    enum class Reason : std::uint8_t
    {
        NoPath,
        Root,
        ContainsHome,
        /**
         * @brief On a server with Windows paths a backslash, or a trailing dot or space, can name another item.
         */
        WindowsName,
        /**
         * @brief The server did not answer in time, so the target could not be compared with the home directory.
         */
        NotVerified,
    };

    Reason reason{Reason::NoPath};
    std::string target{};
    std::string home{};

    /**
     * @brief English text for the log.
     */
    std::string toString() const;

    /**
     * @brief A message built with Utility::localizedMessage that also says nothing was deleted.
     */
    std::string toLocalizedMessage() const;
};

namespace RemoteDeleteGuardDetail
{
    /**
     * @brief True when @p path starts with a drive like "/C:", as OpenSSH for Windows reports paths.
     */
    bool looksLikeWindowsPath(Paths::RemotePath const& path);

    /**
     * @brief True when @p path looks like it lies on a server whose filesystem folds case: a Windows drive path or
     *        one below the /Users of macOS.
     */
    bool looksCaseFolding(Paths::RemotePath const& path);

    /**
     * @brief @ref Paths::RemotePath::isWithin with ASCII letters compared without case.
     */
    bool isWithinIgnoringCase(Paths::RemotePath const& path, Paths::RemotePath const& base);

    /**
     * @brief The checks that need no server: an unset path, the root, a name a Windows server could read as another
     *        one, and @p home within @p target, without ASCII case when the target or the home looks like it lies on a
     *        server that folds case (@ref looksCaseFolding).
     */
    std::optional<RemoteDeleteRefusal>
    refusalByText(Paths::RemotePath const& target, std::optional<Paths::RemotePath> const& home);
}

/**
 * @brief Refuses remote delete targets that are the root, the home directory or a directory containing it, also when
 *        reached through a symlink: the parent of each target is canonicalized by the server, once per parent, and
 *        the target's name appended unresolved, because deleting a link removes the link.
 *
 * One guard serves one request. It runs on the session strand and waits for the SFTP strand.
 */
class RemoteDeleteGuard
{
  public:
    /**
     * @param knownHome The canonical home of the session. Looked up on the first check that needs it while unset,
     *        and stored there once known. While it is unknown the guard logs it and only refuses by text.
     */
    RemoteDeleteGuard(
        SecureShell::SftpSession& sftp,
        std::optional<Paths::RemotePath>& knownHome,
        std::chrono::milliseconds timeout
    );

    std::expected<void, RemoteDeleteRefusal> check(Paths::RemotePath const& target);

    /**
     * @brief The canonical home of the session, looked up like for a check. Nothing when it is unknown or the
     *        server did not answer in time.
     */
    std::optional<Paths::RemotePath> homeDirectory();

    /**
     * @brief Treats every directory below @p root as no symlink, so the canonical form of a path below it is the
     *        canonical @p root with the rest appended. For targets that come from an lstat scan of @p root, which
     *        does not descend into links.
     */
    void trustScannedBelow(Paths::RemotePath root);

  private:
    std::expected<std::optional<Paths::RemotePath>, RemoteDeleteRefusal> home(Paths::RemotePath const& target);
    std::expected<std::optional<Paths::RemotePath>, RemoteDeleteRefusal>
    canonicalDirectory(Paths::RemotePath const& directory, Paths::RemotePath const& target);
    std::expected<std::optional<Paths::RemotePath>, RemoteDeleteRefusal>
    lookUpCanonical(Paths::RemotePath const& directory, Paths::RemotePath const& target);

    SecureShell::SftpSession* sftp_;
    std::optional<Paths::RemotePath>* knownHome_;
    std::chrono::milliseconds timeout_;
    bool homeUnknown_{false};
    std::vector<Paths::RemotePath> scannedRoots_{};
    std::unordered_map<std::string, std::optional<Paths::RemotePath>> canonicalDirectories_{};
};

/**
 * @brief The refusal of the first protected target in @p targets, nothing when every one may be deleted.
 */
template <std::ranges::input_range RangeT, typename ProjectionT = std::identity>
std::optional<RemoteDeleteRefusal>
refuseAnyProtected(RemoteDeleteGuard& guard, RangeT&& targets, ProjectionT projection = {})
{
    std::optional<RemoteDeleteRefusal> refusal{};
    const auto protectedTarget = std::ranges::find_if(
        targets,
        [&guard, &refusal](Paths::RemotePath const& target)
        {
            auto checked = guard.check(target);
            if (!checked)
                refusal = std::move(checked).error();
            return refusal.has_value();
        },
        projection
    );
    if (protectedTarget == std::ranges::end(targets))
        return std::nullopt;
    return refusal;
}
