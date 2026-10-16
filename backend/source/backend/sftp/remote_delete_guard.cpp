#include <backend/sftp/remote_delete_guard.hpp>
#include <log/log.hpp>
#include <ssh/sftp_session.hpp>
#include <utility/localized_message.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <future>
#include <string_view>
#include <utility>

namespace
{
    char foldedCase(char character)
    {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    bool hasWindowsAmbiguousName(Paths::RemotePath const& path)
    {
        return std::ranges::any_of(
            Paths::Detail::splitAtSlashes(path.view()),
            [](std::string_view name)
            {
                return name.find('\\') != std::string_view::npos ||
                    (!name.empty() && (name.back() == '.' || name.back() == ' '));
            }
        );
    }

    bool containsHome(Paths::RemotePath const& target, Paths::RemotePath const& home)
    {
        if (home.isWithin(target))
            return true;
        const auto foldsCase =
            RemoteDeleteGuardDetail::looksCaseFolding(home) || RemoteDeleteGuardDetail::looksCaseFolding(target);
        return foldsCase && RemoteDeleteGuardDetail::isWithinIgnoringCase(home, target);
    }
}

std::string RemoteDeleteRefusal::toString() const
{
    switch (reason)
    {
        case Reason::NoPath:
            return "no path";
        case Reason::Root:
            return fmt::format("'{}' is the root directory", target);
        case Reason::ContainsHome:
            return fmt::format("'{}' is or contains the home directory '{}'", target, home);
        case Reason::WindowsName:
            return fmt::format("'{}' could name another item on a server with Windows paths", target);
        case Reason::NotVerified:
            return fmt::format("'{}' could not be compared with the home directory in time", target);
    }
    return fmt::format("'{}' is protected", target);
}

std::string RemoteDeleteRefusal::toLocalizedMessage() const
{
    switch (reason)
    {
        case Reason::NoPath:
            return Utility::localizedMessage("backend.deleteGuard.noPath");
        case Reason::Root:
            return Utility::localizedMessage("backend.deleteGuard.root", target);
        case Reason::ContainsHome:
            return Utility::localizedMessage("backend.deleteGuard.containsHome", target, home);
        case Reason::WindowsName:
            return Utility::localizedMessage("backend.deleteGuard.windowsName", target);
        case Reason::NotVerified:
            return Utility::localizedMessage("backend.deleteGuard.notVerified", target);
    }
    return Utility::localizedMessage("backend.deleteGuard.notVerified", target);
}

namespace RemoteDeleteGuardDetail
{
    bool looksLikeWindowsPath(Paths::RemotePath const& path)
    {
        const auto text = path.view();
        const auto isLetter = [](char character)
        {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        };
        return text.size() >= 3 && text[0] == '/' && isLetter(text[1]) && text[2] == ':' &&
            (text.size() == 3 || text[3] == '/');
    }

    bool looksCaseFolding(Paths::RemotePath const& path)
    {
        const auto text = path.view();
        return looksLikeWindowsPath(path) || text == "/Users" || text.starts_with("/Users/");
    }

    bool isWithinIgnoringCase(Paths::RemotePath const& path, Paths::RemotePath const& base)
    {
        if (path.empty() || base.empty())
            return false;
        if (base.isRoot())
            return true;
        const auto text = path.view();
        const auto prefix = base.view();
        return text.size() >= prefix.size() &&
            std::ranges::equal(text.substr(0, prefix.size()), prefix, {}, foldedCase, foldedCase) &&
            (text.size() == prefix.size() || text[prefix.size()] == '/');
    }

    std::optional<RemoteDeleteRefusal>
    refusalByText(Paths::RemotePath const& target, std::optional<Paths::RemotePath> const& home)
    {
        if (target.empty())
            return RemoteDeleteRefusal{.reason = RemoteDeleteRefusal::Reason::NoPath};
        if (target.isRoot())
            return RemoteDeleteRefusal{.reason = RemoteDeleteRefusal::Reason::Root, .target = target.value()};
        const auto windowsServer = looksLikeWindowsPath(target) || (home && looksLikeWindowsPath(*home));
        if (windowsServer && hasWindowsAmbiguousName(target))
            return RemoteDeleteRefusal{.reason = RemoteDeleteRefusal::Reason::WindowsName, .target = target.value()};
        if (home && containsHome(target, *home))
        {
            return RemoteDeleteRefusal{
                .reason = RemoteDeleteRefusal::Reason::ContainsHome,
                .target = target.value(),
                .home = home->value(),
            };
        }
        return std::nullopt;
    }
}

RemoteDeleteGuard::RemoteDeleteGuard(
    SecureShell::SftpSession& sftp,
    std::optional<Paths::RemotePath>& knownHome,
    std::chrono::milliseconds timeout
)
    : sftp_{&sftp}
    , knownHome_{&knownHome}
    , timeout_{timeout}
{}

void RemoteDeleteGuard::trustScannedBelow(Paths::RemotePath root)
{
    scannedRoots_.push_back(std::move(root));
}

std::expected<void, RemoteDeleteRefusal> RemoteDeleteGuard::check(Paths::RemotePath const& target)
{
    if (const auto refusal = RemoteDeleteGuardDetail::refusalByText(target, std::nullopt); refusal)
        return std::unexpected(*refusal);

    const auto refuseFor = [&target](std::optional<Paths::RemotePath> const& home)
        -> std::expected<void, RemoteDeleteRefusal>
    {
        if (const auto refusal = RemoteDeleteGuardDetail::refusalByText(target, home); refusal)
            return std::unexpected(*refusal);
        return {};
    };

    return home(target).and_then(
        [&](std::optional<Paths::RemotePath> const& home) -> std::expected<void, RemoteDeleteRefusal>
        {
            if (auto refused = refuseFor(home); !refused || !home)
                return refused;
            return canonicalDirectory(*target.parent(), target)
                .and_then(
                    [&](std::optional<Paths::RemotePath> const& directory) -> std::expected<void, RemoteDeleteRefusal>
                    {
                        if (!directory || !containsHome(*directory / *target.filename(), *home))
                            return {};
                        return std::unexpected(
                            RemoteDeleteRefusal{
                                .reason = RemoteDeleteRefusal::Reason::ContainsHome,
                                .target = target.value(),
                                .home = home->value(),
                            }
                        );
                    }
                );
        }
    );
}

std::optional<Paths::RemotePath> RemoteDeleteGuard::homeDirectory()
{
    return home(Paths::RemotePath{}).value_or(std::nullopt);
}

std::expected<std::optional<Paths::RemotePath>, RemoteDeleteRefusal>
RemoteDeleteGuard::home(Paths::RemotePath const& target)
{
    if (*knownHome_ || homeUnknown_)
        return *knownHome_;

    auto future = sftp_->performPromise(
        [sftp = sftp_]()
        {
            return sftp->canonicalHomeDirectoryInStrand();
        }
    );
    if (future.wait_for(timeout_) != std::future_status::ready)
        return std::unexpected(
            RemoteDeleteRefusal{.reason = RemoteDeleteRefusal::Reason::NotVerified, .target = target.value()}
        );
    auto home = future.get();
    if (!home)
    {
        Log::warn(
            "The remote home directory is unknown ({}), remote deletes only refuse the root directory.",
            home.error().toString()
        );
        homeUnknown_ = true;
        return std::nullopt;
    }
    *knownHome_ = *std::move(home);
    return *knownHome_;
}

std::expected<std::optional<Paths::RemotePath>, RemoteDeleteRefusal>
RemoteDeleteGuard::canonicalDirectory(Paths::RemotePath const& directory, Paths::RemotePath const& target)
{
    const auto scannedRoot = std::ranges::find_if(
        scannedRoots_,
        [&directory](Paths::RemotePath const& root)
        {
            return directory.isWithin(root) && directory != root;
        }
    );
    if (scannedRoot == scannedRoots_.end())
        return lookUpCanonical(directory, target);
    return lookUpCanonical(*scannedRoot, target)
        .transform(
            [&directory, &root = *scannedRoot](std::optional<Paths::RemotePath> const& canonicalRoot)
                -> std::optional<Paths::RemotePath>
            {
                if (!canonicalRoot)
                    return std::nullopt;
                return *canonicalRoot / *directory.relativeTo(root);
            }
        );
}

std::expected<std::optional<Paths::RemotePath>, RemoteDeleteRefusal>
RemoteDeleteGuard::lookUpCanonical(Paths::RemotePath const& directory, Paths::RemotePath const& target)
{
    if (const auto cached = canonicalDirectories_.find(directory.value()); cached != canonicalDirectories_.end())
        return cached->second;

    auto future = sftp_->performPromise(
        [sftp = sftp_, directory]()
        {
            return sftp->canonicalPathInStrand(directory);
        }
    );
    if (future.wait_for(timeout_) != std::future_status::ready)
        return std::unexpected(
            RemoteDeleteRefusal{.reason = RemoteDeleteRefusal::Reason::NotVerified, .target = target.value()}
        );
    auto canonical = future.get();
    if (!canonical)
    {
        // A directory that does not exist holds nothing to delete, the text checks still apply.
        Log::debug("Delete guard: cannot canonicalize '{}': {}", directory.value(), canonical.error().toString());
        return canonicalDirectories_.emplace(directory.value(), std::nullopt).first->second;
    }
    return canonicalDirectories_.emplace(directory.value(), *std::move(canonical)).first->second;
}
