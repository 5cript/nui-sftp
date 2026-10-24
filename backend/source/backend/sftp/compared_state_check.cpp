#include <backend/sftp/compared_state_check.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/rename.hpp>

#include <fmt/format.h>

#include <libssh/sftp.h>

namespace ComparedStateCheck
{
    namespace
    {
        Operation::Error changedError(std::string const& path)
        {
            Log::warn("'{}' changed since it was compared, it is neither replaced nor deleted.", path);
            return Operation::Error{
                .type = Operation::ErrorType::ChangedSinceCompare,
                .extraInfo = fmt::format("'{}' changed after the compare", path),
            };
        }

        SharedData::FileType localTypeOf(
            [[maybe_unused]] std::filesystem::path const& path,
            std::filesystem::file_status status,
            [[maybe_unused]] std::error_code& error
        )
        {
#ifdef _WIN32
            if (std::filesystem::is_symlink(status) || Utility::isLinkOrJunction(path, error))
                return SharedData::FileType::Unknown;
#else
            if (std::filesystem::is_symlink(status))
                return SharedData::FileType::Symlink;
#endif
            return SharedData::fileTypeFromStdFilesystemType(status.type());
        }
    }

    std::uint64_t secondsSinceEpoch(std::filesystem::file_time_type time)
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(time).time_since_epoch())
                .count()
        );
    }

    std::expected<SharedData::ComparedState, std::error_code> localStateOf(std::filesystem::path const& path)
    {
        std::error_code error{};
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
            return SharedData::ComparedState{};
        if (error)
            return std::unexpected(error);

        SharedData::ComparedState state{.exists = true, .type = localTypeOf(path, status, error)};
        if (error)
            return std::unexpected(error);
        switch (state.type)
        {
            case SharedData::FileType::Symlink:
            {
                const auto literal = std::filesystem::read_symlink(path, error);
                if (error)
                    return std::unexpected(error);
                state.linkTarget = Paths::nativeToUtf8(literal);
                return state;
            }
            case SharedData::FileType::Regular:
            {
                state.size = std::filesystem::file_size(path, error);
                if (error)
                    return std::unexpected(error);
                const auto modified = std::filesystem::last_write_time(path, error);
                if (error)
                    return std::unexpected(error);
                state.mtime = secondsSinceEpoch(modified);
                return state;
            }
            default:
                return state;
        }
    }

    std::expected<SharedData::ComparedState, SecureShell::SftpError>
    remoteStateOfInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& path)
    {
        const auto information = sftp.lstatInStrand(path);
        if (!information)
        {
            if (information.error().sftpError == SSH_FX_NO_SUCH_FILE)
                return SharedData::ComparedState{};
            return std::unexpected(information.error());
        }
        auto state = SharedData::comparedStateOf(*information);
        if (state.type != SharedData::FileType::Symlink)
            return state;
        return sftp.readLinkInStrand(path).transform(
            [&state](std::string literal)
            {
                state.linkTarget = std::move(literal);
                return state;
            }
        );
    }

    std::expected<void, Operation::Error>
    requireUnchangedLocal(std::filesystem::path const& path, std::optional<SharedData::ComparedState> const& compared)
    {
        if (!compared)
            return {};
        const auto current = localStateOf(path);
        if (!current)
        {
            return std::unexpected(
                Operation::Error{
                    .type = Operation::ErrorType::FileStatFailed,
                    .extraInfo = fmt::format(
                        "Inspecting '{}': {}", Paths::nativeToUtf8(path), Utility::errorMessage(current.error())
                    ),
                }
            );
        }
        if (!SharedData::unchangedSinceCompare(*compared, *current))
            return std::unexpected(changedError(Paths::nativeToUtf8(path)));
        return {};
    }

    std::expected<void, Operation::Error> requireUnchangedRemoteInStrand(
        SecureShell::SftpSession& sftp,
        Paths::RemotePath const& path,
        std::optional<SharedData::ComparedState> const& compared
    )
    {
        if (!compared)
            return {};
        const auto current = remoteStateOfInStrand(sftp, path);
        if (!current)
            return std::unexpected(Operation::Error{.type = Operation::ErrorType::SftpError, .sftpError = current.error()});
        if (!SharedData::unchangedSinceCompare(*compared, *current))
            return std::unexpected(changedError(path.value()));
        return {};
    }

    std::expected<void, Operation::Error> moveLocalIntoPlace(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        bool replace,
        std::optional<SharedData::ComparedState> const& compared
    )
    {
        return requireUnchangedLocal(to, compared)
            .and_then(
                [&]() -> std::expected<void, Operation::Error>
                {
                    if (const auto moved = Utility::moveIntoPlace(from, to, replace); !moved)
                    {
                        Log::error(
                            "Failed to move '{}' into place: {}",
                            Paths::nativeToUtf8(to),
                            Utility::errorMessage(moved.error())
                        );
                        return std::unexpected(Operation::localMoveError(moved.error()));
                    }
                    return {};
                }
            );
    }

    std::expected<void, Operation::Error> moveRemoteIntoPlaceInStrand(
        SecureShell::SftpSession& sftp,
        Paths::RemotePath const& from,
        Paths::RemotePath const& to,
        bool replace,
        std::string const& asideSuffix,
        SharedData::FileType sourceType,
        std::optional<SharedData::ComparedState> const& compared,
        Log::Toast failureToast
    )
    {
        return requireUnchangedRemoteInStrand(sftp, to, compared)
            .and_then(
                [&]() -> std::expected<void, Operation::Error>
                {
                    const auto moved = sftp.moveIntoPlaceInStrand(from, to, replace, asideSuffix, sourceType);
                    if (!moved)
                    {
                        Log::error(
                            failureToast, "Failed to move '{}' into place: {}", to.value(), moved.error().message
                        );
                        return std::unexpected(Operation::remoteMoveError(moved.error()));
                    }
                    return {};
                }
            );
    }
}
