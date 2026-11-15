#include <backend/sftp/compared_state_check.hpp>
#include <log/log.hpp>
#include <remote_io/remote_file_system_helpers.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/rename.hpp>

#include <fmt/format.h>

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

    namespace
    {
        /**
         * @brief @p information of @p path in the terms of a compare, with the literal of a symlink.
         */
        std::expected<SharedData::ComparedState, RemoteIo::RemoteError> stateOfInStrand(
            RemoteIo::IRemoteFileSystem& fileSystem,
            Paths::RemotePath const& path,
            std::expected<RemoteIo::FileInformation, RemoteIo::RemoteError> const& information
        )
        {
            if (!information)
            {
                if (information.error().kind == RemoteIo::RemoteErrorKind::NoSuchFile)
                    return SharedData::ComparedState{};
                return std::unexpected(information.error());
            }
            auto state = SharedData::comparedStateOf(*information);
            if (state.type != SharedData::FileType::Symlink)
                return state;
            return fileSystem.inStrand(RemoteIo::ReadLink{.path = path})
                .transform(
                    [&state](std::string literal)
                    {
                        state.linkTarget = std::move(literal);
                        return state;
                    }
                );
        }

        /**
         * @brief What is at @p path now, to tell whether it is still @p compared: the probe where the file system
         *        answers one, the lstat where the probe found a difference it may have made itself.
         */
        std::expected<SharedData::ComparedState, RemoteIo::RemoteError> stateToCompareInStrand(
            RemoteIo::IRemoteFileSystem& fileSystem,
            Paths::RemotePath const& path,
            SharedData::ComparedState const& compared
        )
        {
            if (!fileSystem.capabilities().probeEntries)
                return remoteStateOfInStrand(fileSystem, path);
            auto probed = stateOfInStrand(fileSystem, path, fileSystem.inStrand(RemoteIo::ProbeEntry{.path = path}));
            if (!probed || SharedData::unchangedSinceCompare(compared, *probed) || !probeMayMislead(compared, *probed))
                return probed;
            return remoteStateOfInStrand(fileSystem, path);
        }
    }

    std::expected<SharedData::ComparedState, RemoteIo::RemoteError>
    remoteStateOfInStrand(RemoteIo::IRemoteFileSystem& fileSystem, Paths::RemotePath const& path)
    {
        return stateOfInStrand(fileSystem, path, fileSystem.inStrand(RemoteIo::Lstat{.path = path}));
    }

    bool probeMayMislead(SharedData::ComparedState const& compared, SharedData::ComparedState const& probed)
    {
        if (!compared.exists || !probed.exists)
            return false;
        if (compared.type == SharedData::FileType::Symlink)
            return probed.type != SharedData::FileType::Symlink;
        if (compared.type != probed.type || compared.size != probed.size)
            return false;
        return probed.mtime == 0u || compared.mtimePrecision != SharedData::TimePrecision::Second ||
            probed.mtimePrecision != SharedData::TimePrecision::Second;
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
        RemoteIo::IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& path,
        std::optional<SharedData::ComparedState> const& compared
    )
    {
        if (!compared)
            return {};
        const auto current = stateToCompareInStrand(fileSystem, path, *compared);
        if (!current)
            return std::unexpected(
                Operation::Error{.type = Operation::ErrorType::SftpError, .remoteError = current.error()}
            );
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
        RemoteIo::IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& from,
        Paths::RemotePath const& to,
        bool replace,
        std::string const& asideSuffix,
        SharedData::FileType sourceType,
        std::optional<SharedData::ComparedState> const& compared,
        Log::Toast failureToast
    )
    {
        return requireUnchangedRemoteInStrand(fileSystem, to, compared)
            .and_then(
                [&]() -> std::expected<void, Operation::Error>
                {
                    const auto moved =
                        RemoteIo::moveIntoPlaceInStrand(fileSystem, from, to, replace, asideSuffix, sourceType);
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
