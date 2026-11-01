#include <remote_io/remote_file_system_helpers.hpp>
#include <log/log.hpp>
#include <paths/relative_path.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <utility>

namespace RemoteIo
{
    namespace
    {
        bool isMissing(RemoteError const& error)
        {
            return error.kind == RemoteErrorKind::NoSuchFile;
        }

        RemoteError alreadyExists(Paths::RemotePath const& destination)
        {
            return RemoteError{
                .kind = RemoteErrorKind::AlreadyExists,
                .message = fmt::format("'{}' already exists", destination.value()),
            };
        }

        /**
         * @brief @p knownType, or the type an Lstat of @p path finds.
         */
        std::expected<SharedData::FileType, RemoteError> typeOf(
            IRemoteFileSystem& fileSystem,
            Paths::RemotePath const& path,
            std::optional<SharedData::FileType> knownType
        )
        {
            if (knownType.has_value())
                return *knownType;
            return fileSystem.inStrand(Lstat{.path = path})
                .transform(
                    [](FileInformation const& information)
                    {
                        return information.type;
                    }
                );
        }

        /**
         * @brief Renames to a destination that was free, reporting a rename the file system refused because the
         *        destination exists by now as AlreadyExists.
         */
        std::expected<void, RemoteError> renameToFreeName(
            IRemoteFileSystem& fileSystem,
            Paths::RemotePath const& source,
            Paths::RemotePath const& destination
        )
        {
            const auto renamed = fileSystem.inStrand(Rename{.source = source, .destination = destination});
            if (!renamed.has_value() && fileSystem.inStrand(Lstat{.path = destination}).has_value())
                return std::unexpected(alreadyExists(destination));
            return renamed;
        }

        /**
         * @brief Renames @p destination to @p aside, removing a file left at @p aside first when the rename fails.
         */
        std::expected<void, RemoteError>
        moveAside(IRemoteFileSystem& fileSystem, Paths::RemotePath const& destination, Paths::RemotePath const& aside)
        {
            const auto movedAside = fileSystem.inStrand(Rename{.source = destination, .destination = aside});
            if (movedAside.has_value() || !fileSystem.inStrand(Lstat{.path = aside}).has_value())
                return movedAside;

            // A leftover of an earlier replace holds the name.
            if (auto removed = fileSystem.inStrand(RemoveFile{.path = aside}); !removed.has_value())
                return removed;
            return fileSystem.inStrand(Rename{.source = destination, .destination = aside});
        }
    }

    std::expected<void, RemoteError> renameNoReplaceInStrand(
        IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        std::optional<SharedData::FileType> sourceType
    )
    {
        assert(fileSystem.strand()->withinProcessingThread());
        const auto type = typeOf(fileSystem, source, sourceType);
        if (!type.has_value())
            return std::unexpected(type.error());

        if (*type == SharedData::FileType::Regular)
        {
            auto linked = fileSystem.inStrand(LinkIntoPlace{.source = source, .destination = destination});
            if (linked.has_value() || linked.error().kind != RemoteErrorKind::Unsupported)
                return linked;
            Log::debug("RemoteIo: Cannot link '{}': {}, renaming instead.", source.value(), linked.error().toString());
            return renameToFreeName(fileSystem, source, destination);
        }

        if (fileSystem.capabilities().renameSemantics == RenameSemantics::Refuses)
            return renameToFreeName(fileSystem, source, destination);

        // Leaves a window between the check and the rename.
        const auto existing = fileSystem.inStrand(Lstat{.path = destination});
        if (existing.has_value())
            return std::unexpected(alreadyExists(destination));
        if (!isMissing(existing.error()))
            return std::unexpected(existing.error());
        return fileSystem.inStrand(Rename{.source = source, .destination = destination});
    }

    std::expected<void, RemoteError> renameReplaceInStrand(
        IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        std::string_view asideSuffix
    )
    {
        assert(fileSystem.strand()->withinProcessingThread());
        if (fileSystem.capabilities().renameSemantics == RenameSemantics::Replaces)
            return fileSystem.inStrand(Rename{.source = source, .destination = destination});

        // A rename that may refuse an existing destination moves it aside instead of deleting it first.
        const auto aside = destination.withFilenameSuffix(asideSuffix);
        if (!aside)
        {
            return std::unexpected(
                RemoteError{
                    .kind = RemoteErrorKind::Failure,
                    .message = fmt::format("Invalid suffix '{}' for '{}'", asideSuffix, destination.value()),
                    .wrapperError = WrapperErrors::InvalidPath,
                }
            );
        }

        const auto destinationInformation = fileSystem.inStrand(Lstat{.path = destination});
        if (!destinationInformation.has_value())
        {
            if (!isMissing(destinationInformation.error()) || !fileSystem.inStrand(Lstat{.path = *aside}).has_value())
                return fileSystem.inStrand(Rename{.source = source, .destination = destination});
            // A destination that only exists aside, from a rollback that failed, is put back first.
            if (auto restored = fileSystem.inStrand(Rename{.source = *aside, .destination = destination});
                !restored.has_value())
                return restored;
        }
        else if (destinationInformation->isDirectory())
        {
            return std::unexpected(
                RemoteError{
                    .kind = RemoteErrorKind::Failure,
                    .message = fmt::format("'{}' is a directory", destination.value()),
                }
            );
        }

        if (auto movedAside = moveAside(fileSystem, destination, *aside); !movedAside.has_value())
            return movedAside;
        if (auto renamed = fileSystem.inStrand(Rename{.source = source, .destination = destination});
            !renamed.has_value())
        {
            if (auto restored = fileSystem.inStrand(Rename{.source = *aside, .destination = destination});
                !restored.has_value())
            {
                Log::error(
                    "RemoteIo: Could not restore '{}' from '{}': {}",
                    destination.value(),
                    aside->value(),
                    restored.error().toString()
                );
                auto error = renamed.error();
                error.message = fmt::format(
                    "{}. The previous '{}' is kept as '{}'", error.message, destination.value(), aside->value()
                );
                return std::unexpected(std::move(error));
            }
            return renamed;
        }
        if (auto removed = fileSystem.inStrand(RemoveFile{.path = *aside}); !removed.has_value())
            Log::warn("RemoteIo: Could not remove '{}': {}", aside->value(), removed.error().toString());
        return {};
    }

    std::expected<void, RemoteError> moveIntoPlaceInStrand(
        IRemoteFileSystem& fileSystem,
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        bool replace,
        std::string_view asideSuffix,
        std::optional<SharedData::FileType> sourceType
    )
    {
        if (replace)
            return renameReplaceInStrand(fileSystem, source, destination, asideSuffix);
        return renameNoReplaceInStrand(fileSystem, source, destination, sourceType);
    }

    std::expected<std::string, RemoteError>
    readSmallFileInStrand(IRemoteFileSystem& fileSystem, Paths::RemotePath const& path, std::size_t maximumSize)
    {
        const auto opened = fileSystem.inStrand(OpenFile{.path = path, .mode = OpenMode::Read});
        if (!opened.has_value())
            return std::unexpected(opened.error());
        const auto stream = opened->lock();
        if (!stream)
            return std::unexpected(RemoteError{.message = "The file closed before it was read"});
        const auto closeStream = [](IRemoteFileStream* openStream)
        {
            openStream->closeInStrand();
        };
        const std::unique_ptr<IRemoteFileStream, decltype(closeStream)> closer{stream.get(), closeStream};

        std::string content;
        std::array<char, 4096> buffer{};
        const auto pieceSize = std::min(buffer.size(), stream->readLengthLimit());
        while (content.size() < maximumSize)
        {
            const auto bytesRead =
                stream->readSomeInStrand(buffer.data(), std::min(pieceSize, maximumSize - content.size()));
            if (!bytesRead.has_value())
                return std::unexpected(bytesRead.error());
            if (*bytesRead == 0)
                return stream->finishInStrand().transform(
                    [&content]()
                    {
                        return std::move(content);
                    }
                );
            content.append(buffer.data(), *bytesRead);
        }
        return content;
    }

    std::expected<std::vector<Utility::UserDirectory>, RemoteError>
    userDirectoriesInStrand(IRemoteFileSystem& fileSystem, Paths::RemotePath const& home)
    {
        constexpr std::size_t maximumFileSize = 64u * 1024u;
        if (!fileSystem.capabilities().userDirectories)
            return std::vector<Utility::UserDirectory>{};
        // The file below an unset home would be read relative to the server's start directory.
        if (home.empty())
        {
            return std::unexpected(
                RemoteError{
                    .kind = RemoteErrorKind::Failure,
                    .message = "The home directory is not set",
                    .wrapperError = WrapperErrors::InvalidPath,
                }
            );
        }
        const auto file = home / *Paths::RelativePath::make(".config/user-dirs.dirs");
        return readSmallFileInStrand(fileSystem, file, maximumFileSize)
            .transform(
                [&home](std::string const& content)
                {
                    return Utility::parseUserDirectories(content, home.value());
                }
            )
            .or_else(
                [](RemoteError const& error) -> std::expected<std::vector<Utility::UserDirectory>, RemoteError>
                {
                    if (isMissing(error))
                        return std::vector<Utility::UserDirectory>{};
                    return std::unexpected(error);
                }
            );
    }
}
