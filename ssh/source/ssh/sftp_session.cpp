#include <ssh/sftp_session.hpp>
#include <ssh/session.hpp>
#include <ssh/file_information.hpp>
#include <ssh/sftp_error_conversion.hpp>
#include <paths/relative_path.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <memory>
#include <utility>

#include <fcntl.h>

namespace SecureShell
{
    namespace
    {
        bool isMissing(RemoteIo::RemoteError const& error)
        {
            return error.kind == RemoteIo::RemoteErrorKind::NoSuchFile;
        }

        RemoteIo::Capabilities capabilitiesOf(sftp_session session)
        {
            return RemoteIo::Capabilities{
                .symlinks = true,
                .setModificationTime = true,
                .exclusiveCreate = true,
                .appendWrite = true,
                .pipelinedTransfers = true,
                .canonicalPath = true,
                .userDirectories = true,
                .renameSemantics = sftp_extension_supported(session, "posix-rename@openssh.com", "1") == 1
                    ? RemoteIo::RenameSemantics::Replaces
                    : RemoteIo::RenameSemantics::Refuses,
            };
        }

        int openFlagsOf(RemoteIo::OpenMode mode)
        {
            using enum RemoteIo::OpenMode;
            const auto access = RemoteIo::contains(mode, Read | Write) ? O_RDWR
                : RemoteIo::contains(mode, Write)                      ? O_WRONLY
                                                                       : O_RDONLY;
            return access | (RemoteIo::contains(mode, Create) ? O_CREAT : 0) |
                (RemoteIo::contains(mode, Truncate) ? O_TRUNC : 0) | (RemoteIo::contains(mode, Exclusive) ? O_EXCL : 0);
        }
    }

    SftpSession::SftpSession(Session* owner, std::unique_ptr<RemoteIo::ProcessingStrand> strand, NativeSession session)
        : owner_{owner}
        , strand_{std::move(strand)}
        , session_{std::move(session)}
        , bufferProvider_{RemoteIo::DefaultBufferProvider::create()}
        , fileStreams_{}
        , capabilities_{capabilitiesOf(session_.get())}
    {}
    SftpSession::~SftpSession()
    {
        /* close makes no sense, since this lives in a shared_ptr and will only ever end here when it was already
         * removed */
    }
    bool SftpSession::close()
    {
        return close(false);
    }
    bool SftpSession::close(bool isBackElement)
    {
        if (strand_->isFinalized())
            return false;

        const auto removeItself = [this, isBackElement]()
        {
            removeAllFileStreams();
            // Here and not in the destructor, which runs on whichever thread drops the last reference.
            session_.reset();
            owner_->sftpSessionRemoveItself(this, isBackElement);
            return true;
        };
        if (!strand_->withinProcessingThread())
            return strand_->pushFinalPromiseTask(removeItself).get();

        // The final task would wait for this thread. Removing itself may drop the last owner under the strand's lock.
        const auto self = shared_from_this();
        bool removed = false;
        strand_->doFinalSync(
            [&removed, &removeItself]()
            {
                removed = removeItself();
            }
        );
        return removed;
    }
    void SftpSession::fileStreamRemoveItself(FileStream* stream, bool isBackElement)
    {
        if (isBackElement && fileStreams_.back().get() == stream)
        {
            fileStreams_.pop_back();
        }
        else
        {
            fileStreams_.erase(
                std::remove_if(
                    fileStreams_.begin(),
                    fileStreams_.end(),
                    [stream](auto const& item)
                    {
                        return item.get() == stream;
                    }
                ),
                fileStreams_.end()
            );
        }
    }
    void SftpSession::removeAllFileStreams()
    {
        while (!fileStreams_.empty())
        {
            fileStreams_.back()->close(true);
        }
    }
    void SftpSession::dispatch(std::function<void()> start)
    {
        strand_->pushTask(std::move(start));
    }

    RemoteIo::OperationResult<RemoteIo::ListDirectory> SftpSession::run(RemoteIo::ListDirectory const& operation)
    {
        auto const& path = operation.path;
        int closeResult = 0;
        std::vector<FileInformation> entries{};

        {
            std::unique_ptr<sftp_dir_struct, std::function<void(sftp_dir_struct*)>> dir{
                sftp_opendir(session_.get(), path.c_str()),
                [&](sftp_dir_struct* dir)
                {
                    if (dir != nullptr)
                    {
                        closeResult = sftp_closedir(dir);
                    }
                }
            };
            if (dir == nullptr)
            {
                return std::unexpected(lastError());
            }

            {
                std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> entry{
                    sftp_readdir(session_.get(), dir.get()), sftp_attributes_free
                };

                for (; entry != nullptr; entry.reset(sftp_readdir(session_.get(), dir.get())))
                {
                    entries.push_back(fromSftpAttributes(entry.get()));
                }
            }

            if (!sftp_dir_eof(dir.get()))
            {
                return std::unexpected(lastError());
            }
        }
        if (closeResult != SSH_OK)
        {
            return std::unexpected(lastError());
        }

        for (auto& entry : entries)
        {
            if (operation.resolveLinks && entry.type == SharedData::FileType::Symlink)
            {
                // A name that is no valid segment, like "x/..", must not become a path to look up.
                const auto name = entry.nameSegment();
                if (!name)
                    continue;
                const auto fullPath = path / *name;

                std::unique_ptr<char, decltype(&ssh_string_free_char)> linkLiteral{
                    sftp_readlink(session_.get(), fullPath.c_str()), ssh_string_free_char
                };
                if (linkLiteral)
                {
                    const auto* raw = static_cast<const char*>(linkLiteral.get());
                    entry.linkTarget = Paths::sanitizedUtf8(raw ? raw : "");
                }

                std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> targetAttrs{
                    sftp_stat(session_.get(), fullPath.c_str()), sftp_attributes_free
                };
                if (targetAttrs != nullptr)
                    entry.resolvedTarget =
                        std::make_shared<SharedData::DirectoryEntry>(fromSftpAttributes(targetAttrs.get()));
            }
        }

        return entries;
    }

    std::expected<std::vector<FileInformation>, SftpSession::Error>
    SftpSession::listDirectoryInStrand(Paths::RemotePath const& path, bool resolveLinks)
    {
        return runInStrand(RemoteIo::ListDirectory{.path = path, .resolveLinks = resolveLinks});
    }

    std::future<std::expected<std::vector<FileInformation>, SftpSession::Error>>
    SftpSession::listDirectory(Paths::RemotePath const& path, bool resolveLinks)
    {
        return later(RemoteIo::ListDirectory{.path = path, .resolveLinks = resolveLinks});
    }

    RemoteIo::OperationResult<RemoteIo::CreateNewDirectory>
    SftpSession::run(RemoteIo::CreateNewDirectory const& operation)
    {
        const auto result = sftp_mkdir(session_.get(),
            operation.path.c_str(),
            static_cast<unsigned long>(operation.permissions & std::filesystem::perms::mask)
        );
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createDirectoryInStrand(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return runInStrand(RemoteIo::CreateNewDirectory{.path = path, .permissions = permissions});
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createDirectory(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return later(RemoteIo::CreateNewDirectory{.path = path, .permissions = permissions});
    }

    RemoteIo::OperationResult<RemoteIo::CreateDirectoryIfItDoesntExist>
    SftpSession::run(RemoteIo::CreateDirectoryIfItDoesntExist const& operation)
    {
        auto const& path = operation.path;
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_lstat(session_.get(), path.c_str()), sftp_attributes_free
        };
        if (!attributes)
        {
            const auto result = sftp_mkdir(session_.get(), path.c_str(), static_cast<unsigned long>(operation.permissions & std::filesystem::perms::mask)
            );
            if (result != SSH_OK)
                return std::unexpected(lastError());
        }
        else
        {
            if (attributes->type != SSH_FILEXFER_TYPE_DIRECTORY)
            {
                return std::unexpected(toRemoteError(
                    SftpError{
                        .message = "Path exists and is not a directory",
                        .sshError = SSH_OK,
                        .sftpError = SSH_FX_FILE_ALREADY_EXISTS,
                    }
                ));
            }
        }
        return {};
    }

    std::expected<void, SftpSession::Error> SftpSession::createDirectoryIfItDoesntExistInStrand(
        Paths::RemotePath const& path,
        std::filesystem::perms permissions
    )
    {
        return runInStrand(RemoteIo::CreateDirectoryIfItDoesntExist{.path = path, .permissions = permissions});
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createDirectoryIfItDoesntExist(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return later(RemoteIo::CreateDirectoryIfItDoesntExist{.path = path, .permissions = permissions});
    }

    RemoteIo::OperationResult<RemoteIo::CreateEmptyFile> SftpSession::run(RemoteIo::CreateEmptyFile const& operation)
    {
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file_struct*)>> file{
            // O_EXCL: an existing file is an error, not reported as created.
            sftp_open(session_.get(),
                operation.path.c_str(),
                O_CREAT | O_EXCL,
                static_cast<unsigned long>(operation.permissions & std::filesystem::perms::mask)
            ),
            [&](sftp_file_struct* file)
            {
                if (file != nullptr)
                {
                    sftp_close(file);
                }
            }
        };

        if (file == nullptr)
            return std::unexpected(lastError());

        return {};
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createFileInStrand(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return runInStrand(RemoteIo::CreateEmptyFile{.path = path, .permissions = permissions});
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createFile(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return later(RemoteIo::CreateEmptyFile{.path = path, .permissions = permissions});
    }

    RemoteIo::OperationResult<RemoteIo::RemoveFile> SftpSession::run(RemoteIo::RemoveFile const& operation)
    {
        const auto result = sftp_unlink(session_.get(), operation.path.c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::expected<void, SftpSession::Error> SftpSession::removeFileInStrand(Paths::RemotePath const& path)
    {
        return runInStrand(RemoteIo::RemoveFile{.path = path});
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeFile(Paths::RemotePath const& path)
    {
        return later(RemoteIo::RemoveFile{.path = path});
    }

    RemoteIo::OperationResult<RemoteIo::RemoveEmptyDirectory>
    SftpSession::run(RemoteIo::RemoveEmptyDirectory const& operation)
    {
        const auto result = sftp_rmdir(session_.get(), operation.path.c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::expected<void, SftpSession::Error> SftpSession::removeDirectoryInStrand(Paths::RemotePath const& path)
    {
        return runInStrand(RemoteIo::RemoveEmptyDirectory{.path = path});
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeDirectory(Paths::RemotePath const& path)
    {
        return later(RemoteIo::RemoveEmptyDirectory{.path = path});
    }

    RemoteIo::OperationResult<RemoteIo::FilterOutEmptyDirectories>
    SftpSession::run(RemoteIo::FilterOutEmptyDirectories const& operation)
    {
        std::vector<Paths::RemotePath> nonEmpties;
        for (const auto& path : operation.directories)
        {
            std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                sftp_lstat(session_.get(), path.c_str()), sftp_attributes_free
            };

            if (attributes == nullptr)
                return std::unexpected(lastError());

            if (attributes->type != SSH_FILEXFER_TYPE_DIRECTORY)
                continue;

            int closeResult = SSH_OK;
            bool nonEmpty = false;
            {
                std::unique_ptr<sftp_dir_struct, std::function<void(sftp_dir_struct*)>> dir{
                    sftp_opendir(session_.get(), path.c_str()),
                    [&closeResult](sftp_dir_struct* dir)
                    {
                        if (dir != nullptr)
                        {
                            closeResult = sftp_closedir(dir);
                        }
                    }
                };
                if (dir == nullptr)
                {
                    return std::unexpected(lastError());
                }

                std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> entry{
                    sftp_readdir(session_.get(), dir.get()), sftp_attributes_free
                };
                using namespace std::string_literals;

                auto findNonDotEntry = [this, &entry, &dir]()
                {
                    if (entry == nullptr)
                        return false;
                    if (entry->name == ".."s || entry->name == "."s)
                    {
                        entry.reset(sftp_readdir(session_.get(), dir.get()));
                        return false;
                    }
                    else
                        return true;
                };

                nonEmpty = findNonDotEntry();
                if (!nonEmpty)
                    nonEmpty = findNonDotEntry();
                if (!nonEmpty)
                    nonEmpty = findNonDotEntry();
            }
            if (closeResult != SSH_OK)
                return std::unexpected(lastError());
            if (nonEmpty)
                nonEmpties.push_back(path);
        }
        return nonEmpties;
    }

    std::future<std::expected<std::vector<Paths::RemotePath>, SftpSession::Error>>
    SftpSession::filterOutEmptyDirectories(std::vector<Paths::RemotePath> directories)
    {
        return later(RemoteIo::FilterOutEmptyDirectories{.directories = std::move(directories)});
    }

    RemoteIo::OperationResult<RemoteIo::RemoveAll> SftpSession::run(RemoteIo::RemoveAll const& operation)
    {
        for (const auto& path : operation.paths)
        {
            std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                sftp_lstat(session_.get(), path.c_str()), sftp_attributes_free
            };

            if (attributes == nullptr)
                return std::unexpected(lastError());

            if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
            {
                const auto result = sftp_rmdir(session_.get(), path.c_str());
                if (result != SSH_OK)
                    return std::unexpected(lastError());
            }
            else
            {
                const auto result = sftp_unlink(session_.get(), path.c_str());
                if (result != SSH_OK)
                    return std::unexpected(lastError());
            }
        }
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeAll(std::vector<Paths::RemotePath> paths)
    {
        return later(RemoteIo::RemoveAll{.paths = std::move(paths)});
    }

    RemoteIo::OperationResult<RemoteIo::Stat> SftpSession::run(RemoteIo::Stat const& operation)
    {
        const auto callStart = std::chrono::steady_clock::now();
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_stat(session_.get(), operation.path.c_str()), sftp_attributes_free
        };
        chunkController_.recordRoundTrip(std::chrono::steady_clock::now() - callStart);
        if (attributes == nullptr)
            return std::unexpected(lastError());

        return fromSftpAttributes(attributes.get());
    }

    std::expected<FileInformation, SftpSession::Error> SftpSession::statInStrand(Paths::RemotePath const& path)
    {
        return runInStrand(RemoteIo::Stat{.path = path});
    }

    std::future<std::expected<FileInformation, SftpSession::Error>> SftpSession::stat(Paths::RemotePath const& path)
    {
        return later(RemoteIo::Stat{.path = path});
    }

    RemoteIo::OperationResult<RemoteIo::Lstat> SftpSession::run(RemoteIo::Lstat const& operation)
    {
        const auto callStart = std::chrono::steady_clock::now();
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_lstat(session_.get(), operation.path.c_str()), sftp_attributes_free
        };
        chunkController_.recordRoundTrip(std::chrono::steady_clock::now() - callStart);
        if (attributes == nullptr)
            return std::unexpected(lastError());

        return fromSftpAttributes(attributes.get());
    }

    std::expected<FileInformation, SftpSession::Error> SftpSession::lstatInStrand(Paths::RemotePath const& path)
    {
        return runInStrand(RemoteIo::Lstat{.path = path});
    }

    std::future<std::expected<FileInformation, SftpSession::Error>>
    SftpSession::lstat(Paths::RemotePath const& path)
    {
        return later(RemoteIo::Lstat{.path = path});
    }

    std::expected<void, SftpSession::Error>
    SftpSession::statInStrand(Paths::RemotePath const& path, sftp_attributes attributes)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const auto result = sftp_setstat(session_.get(), path.c_str(), attributes);
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::stat(Paths::RemotePath const& path, sftp_attributes attributes)
    {
        return performPromise(
            [this, path, attributes]()
            {
                return statInStrand(path, attributes);
            }
        );
    }

    RemoteIo::OperationResult<RemoteIo::SetModificationTime>
    SftpSession::run(RemoteIo::SetModificationTime const& operation)
    {
        // SFTP version 3 has times in whole unsigned 32 bit seconds and sets the access time along with the
        // modification time.
        const auto seconds =
            std::chrono::floor<std::chrono::seconds>(operation.modificationTime.time_since_epoch()).count();
        if (!std::in_range<std::uint32_t>(seconds))
        {
            return std::unexpected(toRemoteError(
                SftpError{
                    .message = fmt::format(
                        "SFTP cannot set a modification time before 1970 or after 2106 on '{}'", operation.path.value()
                    ),
                    .sftpError = SSH_FX_OP_UNSUPPORTED,
                }
            ));
        }
        sftp_attributes_struct attributes{};
        attributes.flags = SSH_FILEXFER_ATTR_ACMODTIME;
        attributes.atime = static_cast<std::uint32_t>(seconds);
        attributes.mtime = static_cast<std::uint32_t>(seconds);
        if (sftp_setstat(session_.get(), operation.path.c_str(), &attributes) != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::expected<void, SftpSession::Error> SftpSession::setModificationTimeInStrand(
        Paths::RemotePath const& path,
        std::chrono::system_clock::time_point modificationTime
    )
    {
        return runInStrand(RemoteIo::SetModificationTime{.path = path, .modificationTime = modificationTime});
    }

    RemoteIo::OperationResult<RemoteIo::Rename> SftpSession::run(RemoteIo::Rename const& operation)
    {
        const auto result = sftp_rename(session_.get(), operation.source.c_str(), operation.destination.c_str());
        if (result != SSH_OK)
        {
            return std::unexpected(lastError());
        }
        return {};
    }

    std::expected<void, SftpSession::Error>
    SftpSession::renameInStrand(Paths::RemotePath const& source, Paths::RemotePath const& destination)
    {
        return runInStrand(RemoteIo::Rename{.source = source, .destination = destination});
    }

    bool SftpSession::supportsExtensionInStrand(char const* name, char const* version) const
    {
        assert(strand_->withinProcessingThread());
        return session_ && sftp_extension_supported(session_.get(), name, version) == 1;
    }

    std::expected<void, SftpSession::Error> SftpSession::renameNoReplaceInStrand(
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        std::optional<SharedData::FileType> sourceType
    )
    {
        assert(strand_->withinProcessingThread());
        const auto type = sourceType.has_value()
            ? std::expected<SharedData::FileType, Error>{*sourceType}
            : lstatInStrand(source).transform(
                  [](FileInformation const& information)
                  {
                      return information.type;
                  }
              );
        if (!type.has_value())
            return std::unexpected(type.error());

        if (*type == SharedData::FileType::Regular && supportsExtensionInStrand("hardlink@openssh.com"))
            return hardlinkIntoPlaceInStrand(source, destination);

        // libssh sends posix-rename@openssh.com when the server offers it, which replaces, while a plain SFTP v3
        // rename refuses an existing destination.
        if (!supportsExtensionInStrand("posix-rename@openssh.com"))
        {
            const auto renamed = renameInStrand(source, destination);
            if (!renamed.has_value() && lstatInStrand(destination).has_value())
                return std::unexpected(alreadyExists(destination));
            return renamed;
        }

        // Leaves a window between the check and the rename.
        if (lstatInStrand(destination).has_value())
            return std::unexpected(alreadyExists(destination));
        return renameInStrand(source, destination);
    }

    RemoteIo::OperationResult<RemoteIo::LinkIntoPlace> SftpSession::run(RemoteIo::LinkIntoPlace const& operation)
    {
        auto const& source = operation.source;
        auto const& destination = operation.destination;
        if (!supportsExtensionInStrand("hardlink@openssh.com"))
        {
            return std::unexpected(toRemoteError(
                SftpError{
                    .message = "The server has no hardlink@openssh.com",
                    .sftpError = SSH_FX_OP_UNSUPPORTED,
                }
            ));
        }

        if (sftp_hardlink(session_.get(), source.c_str(), destination.c_str()) == SSH_OK)
        {
            if (auto removed = run(RemoteIo::RemoveFile{.path = source}); !removed.has_value())
            {
                Log::warn(
                    "SftpSession: Moved '{}' to '{}' but could not remove '{}': {}",
                    source.value(),
                    destination.value(),
                    source.value(),
                    removed.error().toString()
                );
            }
            return {};
        }

        auto linkError = lastError();
        const auto existing = run(RemoteIo::Lstat{.path = destination});
        if (existing.has_value())
            return std::unexpected(alreadyExists(destination));
        // Filesystems without hard links, and Linux protected_hardlinks, refuse the link.
        if (isMissing(existing.error()))
            linkError.kind = RemoteIo::RemoteErrorKind::Unsupported;
        // Unsupported promises a free destination, which the failed lstat could not confirm.
        else if (linkError.kind == RemoteIo::RemoteErrorKind::Unsupported)
            return std::unexpected(existing.error());
        return std::unexpected(std::move(linkError));
    }

    std::expected<void, SftpSession::Error>
    SftpSession::hardlinkIntoPlaceInStrand(Paths::RemotePath const& source, Paths::RemotePath const& destination)
    {
        auto linked = runInStrand(RemoteIo::LinkIntoPlace{.source = source, .destination = destination});
        if (linked.has_value() || linked.error().kind != RemoteIo::RemoteErrorKind::Unsupported)
            return linked;
        Log::debug("SftpSession: Cannot link '{}': {}, renaming instead.", source.value(), linked.error().toString());
        return renameInStrand(source, destination);
    }

    std::expected<void, SftpSession::Error> SftpSession::renameReplaceInStrand(
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        std::string_view asideSuffix
    )
    {
        assert(strand_->withinProcessingThread());
        if (supportsExtensionInStrand("posix-rename@openssh.com"))
            return renameInStrand(source, destination);

        // Plain SFTP rename refuses an existing destination, so it is moved aside instead of deleted first.
        const auto aside = destination.withFilenameSuffix(asideSuffix);
        if (!aside)
        {
            return std::unexpected(toRemoteError(
                SftpError{
                    .message = fmt::format("Invalid suffix '{}' for '{}'", asideSuffix, destination.value()),
                    .wrapperError = WrapperErrors::InvalidPath,
                }
            ));
        }

        const auto destinationInfo = lstatInStrand(destination);
        if (!destinationInfo.has_value())
        {
            if (!isMissing(destinationInfo.error()) || !lstatInStrand(*aside).has_value())
                return renameInStrand(source, destination);
            // A destination that only exists aside, from a rollback that failed, is put back first.
            if (auto restored = renameInStrand(*aside, destination); !restored.has_value())
                return restored;
        }
        else if (destinationInfo->isDirectory())
        {
            return std::unexpected(toRemoteError(
                SftpError{
                    .message = fmt::format("'{}' is a directory", destination.value()),
                    .sftpError = SSH_FX_FAILURE,
                }
            ));
        }

        if (auto movedAside = moveAsideInStrand(destination, *aside); !movedAside.has_value())
            return movedAside;
        if (auto renamed = renameInStrand(source, destination); !renamed.has_value())
        {
            if (auto restored = renameInStrand(*aside, destination); !restored.has_value())
            {
                Log::error(
                    "SftpSession: Could not restore '{}' from '{}': {}",
                    destination.value(),
                    aside->value(),
                    restored.error().toString()
                );
                auto error = renamed.error();
                error.message =
                    fmt::format("{}. The previous '{}' is kept as '{}'", error.message, destination.value(), aside->value());
                return std::unexpected(std::move(error));
            }
            return renamed;
        }
        if (auto removed = removeFileInStrand(*aside); !removed.has_value())
            Log::warn("SftpSession: Could not remove '{}': {}", aside->value(), removed.error().toString());
        return {};
    }

    std::expected<void, SftpSession::Error>
    SftpSession::moveAsideInStrand(Paths::RemotePath const& destination, Paths::RemotePath const& aside)
    {
        const auto movedAside = renameInStrand(destination, aside);
        if (movedAside.has_value() || !lstatInStrand(aside).has_value())
            return movedAside;

        // A leftover of an earlier replace holds the name.
        if (auto removed = removeFileInStrand(aside); !removed.has_value())
            return removed;
        return renameInStrand(destination, aside);
    }

    std::expected<void, SftpSession::Error> SftpSession::moveIntoPlaceInStrand(
        Paths::RemotePath const& source,
        Paths::RemotePath const& destination,
        bool replace,
        std::string_view asideSuffix,
        std::optional<SharedData::FileType> sourceType
    )
    {
        if (replace)
            return renameReplaceInStrand(source, destination, asideSuffix);
        return renameNoReplaceInStrand(source, destination, sourceType);
    }

    SftpSession::Error SftpSession::alreadyExists(Paths::RemotePath const& destination)
    {
        return toRemoteError(
            SftpError{
                .message = fmt::format("'{}' already exists", destination.value()),
                .sftpError = SSH_FX_FILE_ALREADY_EXISTS,
            }
        );
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::rename(Paths::RemotePath const& source, Paths::RemotePath const& destination)
    {
        return later(RemoteIo::Rename{.source = source, .destination = destination});
    }

    std::expected<void, SftpSession::Error>
    SftpSession::chownInStrand(Paths::RemotePath const& path, uid_t owner, gid_t group)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const auto result = sftp_chown(session_.get(), path.c_str(), owner, group);
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::chown(Paths::RemotePath const& path, uid_t owner, gid_t group)
    {
        return performPromise(
            [this, path, owner, group]()
            {
                return chownInStrand(path, owner, group);
            }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::chmodInStrand(Paths::RemotePath const& path, std::filesystem::perms perms)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const auto result = sftp_chmod(session_.get(), path.c_str(), static_cast<mode_t>(perms));
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::chmod(Paths::RemotePath const& path, std::filesystem::perms perms)
    {
        return performPromise(
            [this, path, perms]()
            {
                return chmodInStrand(path, perms);
            }
        );
    }

    std::expected<Paths::RemotePath, SftpSession::Error> SftpSession::canonicalizeInStrand(char const* path)
    {
        assert(strand_->withinProcessingThread());
        const std::unique_ptr<char, decltype(&ssh_string_free_char)> canonical{
            sftp_canonicalize_path(session_.get(), path), ssh_string_free_char
        };
        if (canonical == nullptr)
            return std::unexpected(lastError());
        return Paths::RemotePath::make(canonical.get())
            .transform_error(
                [](Paths::PathError const& error)
                {
                    return toRemoteError(
                        SftpError{
                            .message = fmt::format("The server canonicalized to {}", error.toString()),
                            .sftpError = SSH_FX_FAILURE,
                            .wrapperError = WrapperErrors::InvalidPath,
                        }
                    );
                }
            );
    }

    RemoteIo::OperationResult<RemoteIo::CanonicalHomeDirectory>
    SftpSession::run(RemoteIo::CanonicalHomeDirectory const&)
    {
        return canonicalizeInStrand(".");
    }

    std::expected<Paths::RemotePath, SftpSession::Error> SftpSession::canonicalHomeDirectoryInStrand()
    {
        return runInStrand(RemoteIo::CanonicalHomeDirectory{});
    }

    RemoteIo::OperationResult<RemoteIo::CanonicalPath> SftpSession::run(RemoteIo::CanonicalPath const& operation)
    {
        return canonicalizeInStrand(operation.path.c_str());
    }

    std::expected<Paths::RemotePath, SftpSession::Error>
    SftpSession::canonicalPathInStrand(Paths::RemotePath const& path)
    {
        return runInStrand(RemoteIo::CanonicalPath{.path = path});
    }

    RemoteIo::OperationResult<RemoteIo::HomeDirectory> SftpSession::run(RemoteIo::HomeDirectory const&)
    {
        if (auto home = run(RemoteIo::CanonicalHomeDirectory{}); home)
            return home;
        if (!session_)
            return std::unexpected(lastError());

        char* rawUser = nullptr;
        const auto optionResult = ssh_options_get(session_->session, SSH_OPTIONS_USER, &rawUser);
        const std::unique_ptr<char, decltype(&ssh_string_free_char)> user{rawUser, ssh_string_free_char};
        if (optionResult != SSH_OK || user == nullptr || user.get()[0] == '\0')
            return std::unexpected(
                toRemoteError(SftpError{.message = "Server did not report a home directory and the user is unknown"})
            );

        const auto guess = Paths::RemotePath::make(fmt::format("/home/{}", user.get()));
        if (!guess)
            return Paths::RemotePath::root();
        const auto information = run(RemoteIo::Stat{.path = *guess});
        if (information && information->isDirectory())
            return *guess;
        return Paths::RemotePath::root();
    }

    std::expected<Paths::RemotePath, SftpSession::Error> SftpSession::homeDirectoryInStrand()
    {
        return runInStrand(RemoteIo::HomeDirectory{});
    }

    std::future<std::expected<Paths::RemotePath, SftpSession::Error>> SftpSession::homeDirectory()
    {
        return later(RemoteIo::HomeDirectory{});
    }

    std::expected<std::string, SftpSession::Error>
    SftpSession::readSmallFileInStrand(Paths::RemotePath const& path, std::size_t maximumSize)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const std::unique_ptr<sftp_file_struct, decltype(&sftp_close)> file{
            sftp_open(session_.get(), path.c_str(), O_RDONLY, 0), sftp_close
        };
        if (file == nullptr)
            return std::unexpected(lastError());

        std::string content;
        std::array<char, 4096> buffer{};
        while (content.size() < maximumSize)
        {
            const auto bytesRead =
                sftp_read(file.get(), buffer.data(), std::min(buffer.size(), maximumSize - content.size()));
            if (bytesRead < 0)
                return std::unexpected(lastError());
            if (bytesRead == 0)
                break;
            content.append(buffer.data(), static_cast<std::size_t>(bytesRead));
        }
        return content;
    }

    std::expected<std::vector<Utility::UserDirectory>, SftpSession::Error>
    SftpSession::userDirectoriesInStrand(Paths::RemotePath const& home)
    {
        constexpr std::size_t maximumFileSize = 64u * 1024u;
        if (auto valid = requireSetInStrand(home); !valid)
            return std::unexpected(valid.error());
        return readSmallFileInStrand(home / *Paths::RelativePath::make(".config/user-dirs.dirs"), maximumFileSize)
            .transform(
                [&home](std::string const& content)
                {
                    return Utility::parseUserDirectories(content, home.value());
                }
            )
            .or_else(
                [](Error const& error) -> std::expected<std::vector<Utility::UserDirectory>, Error>
                {
                    if (isMissing(error))
                        return std::vector<Utility::UserDirectory>{};
                    return std::unexpected(error);
                }
            );
    }

    std::future<std::expected<std::vector<Utility::UserDirectory>, SftpSession::Error>>
    SftpSession::userDirectories(Paths::RemotePath const& home)
    {
        return performPromise(
            [this, home]()
            {
                return userDirectoriesInStrand(home);
            }
        );
    }

    SftpSession::Error SftpSession::unsetPathError()
    {
        return toRemoteError(
            SftpError{
                .message = "The remote path is not set",
                .sftpError = SSH_FX_FAILURE,
                .wrapperError = WrapperErrors::InvalidPath,
            }
        );
    }

    SftpSession::Error SftpSession::lastError() const
    {
        if (!session_)
            return toRemoteError(
                SftpError{.message = "The SFTP session is closed", .sftpError = SSH_FX_CONNECTION_LOST}
            );
        // libssh keeps the status of the last answer when the connection breaks.
        if ((ssh_get_status(session_->session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) != 0 ||
            ssh_channel_is_open(session_->channel) == 0)
        {
            return toRemoteError(
                SftpError{
                    .message = ssh_get_error(session_->session),
                    .sshError = ssh_get_error_code(session_->session),
                    .sftpError = SSH_FX_CONNECTION_LOST,
                }
            );
        }
        return toRemoteError(SftpError{.sftpError = sftp_get_error(session_.get())});
    }

    std::expected<sftp_limits_struct, SftpSession::Error> SftpSession::limitsInStrand()
    {
        assert(strand_->withinProcessingThread());
        // A copy the caller frees.
        const std::unique_ptr<sftp_limits_struct, decltype(&sftp_limits_free)> limits{
            sftp_limits(session_.get()), &sftp_limits_free
        };
        if (!limits)
            return std::unexpected(lastError());
        return *limits;
    }

    std::future<std::expected<sftp_limits_struct, SftpSession::Error>> SftpSession::limits()
    {
        return performPromise(
            [this]()
            {
                return limitsInStrand();
            }
        );
    }

    RemoteIo::OperationResult<RemoteIo::ReadTransferLimits> SftpSession::run(RemoteIo::ReadTransferLimits const&)
    {
        return limitsInStrand().transform(
            [](sftp_limits_struct const& limits)
            {
                return RemoteIo::TransferLimits{.maxWriteLength = limits.max_write_length};
            }
        );
    }

    std::expected<std::shared_ptr<FileStream>, SftpSession::Error>
    SftpSession::openStreamInStrand(Paths::RemotePath const& path, int flags, std::filesystem::perms permissions)
    {
        const auto callStart = std::chrono::steady_clock::now();
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file_struct*)>> file{
            sftp_open(session_.get(), path.c_str(), flags, static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
            ),
            [&](sftp_file_struct* file)
            {
                if (file != nullptr)
                {
                    sftp_close(file);
                }
            }
        };
        chunkController_.recordRoundTrip(std::chrono::steady_clock::now() - callStart);

        if (!file)
            return std::unexpected(lastError());

        const auto limits = limitsInStrand();
        if (!limits)
            return std::unexpected(limits.error());

        auto stream = std::make_shared<FileStream>(shared_from_this(), file.release(), *limits);
        fileStreams_.push_back(stream);
        return stream;
    }

    RemoteIo::OperationResult<RemoteIo::OpenFile> SftpSession::run(RemoteIo::OpenFile const& operation)
    {
        return openStreamInStrand(operation.path, openFlagsOf(operation.mode), operation.permissions)
            .transform(
                [](std::shared_ptr<FileStream> const& stream)
                {
                    return std::weak_ptr<RemoteIo::IRemoteFileStream>{stream};
                }
            );
    }

    std::expected<std::weak_ptr<FileStream>, SftpSession::Error>
    SftpSession::openFileInStrand(Paths::RemotePath const& path, OpenType openType, std::filesystem::perms permissions)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        return openStreamInStrand(path, static_cast<int>(openType), permissions)
            .transform(
                [](std::shared_ptr<FileStream> const& stream)
                {
                    return std::weak_ptr<FileStream>{stream};
                }
            );
    }

    std::future<std::expected<std::weak_ptr<FileStream>, SftpSession::Error>>
    SftpSession::openFile(Paths::RemotePath const& path, OpenType openType, std::filesystem::perms permissions)
    {
        return performPromise(
            [this, path, openType, permissions]()
            {
                return openFileInStrand(path, openType, permissions);
            }
        );
    }

    RemoteIo::OperationResult<RemoteIo::ReadLinkDeep> SftpSession::run(RemoteIo::ReadLinkDeep const& operation)
    {
        auto currentPath = operation.path;
        std::optional<std::string> linkContent{};
        for (int depth = 0; depth < operation.maxDepth; ++depth)
        {
            std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                sftp_lstat(session_.get(), currentPath.c_str()), sftp_attributes_free
            };
            if (attributes == nullptr)
            {
                auto last = lastError();
                if (isMissing(last))
                {
                    return DeepLinkResult{
                        .linkContent = linkContent.value_or(""),
                        .linkTarget = currentPath,
                        .targetInfo = std::nullopt,
                    };
                }
                return std::unexpected(std::move(last));
            }

            if (attributes->type != SSH_FILEXFER_TYPE_SYMLINK)
            {
                return DeepLinkResult{
                    .linkContent = linkContent.value_or(""),
                    .linkTarget = currentPath,
                    .targetInfo = fromSftpAttributes(attributes.get()),
                };
            }

            const auto readLinkResult = std::unique_ptr<char, decltype(&ssh_string_free_char)>(
                sftp_readlink(session_.get(), currentPath.c_str()), ssh_string_free_char
            );
            if (readLinkResult == nullptr)
                return std::unexpected(lastError());
            if (!linkContent)
                linkContent = readLinkResult.get();

            // Relative content is relative to the link's directory, not to the server's working directory.
            auto next = currentPath.resolveLink(readLinkResult.get());
            if (!next)
            {
                return std::unexpected(toRemoteError(
                    SftpError{
                        .message = fmt::format(
                            "Symlink '{}' has an unusable target: {}", currentPath.value(), next.error().toString()
                        ),
                        .sftpError = SSH_FX_FAILURE,
                        .wrapperError = WrapperErrors::InvalidPath,
                    }
                ));
            }
            currentPath = *std::move(next);
        }

        return std::unexpected(toRemoteError(
            SftpError{
                .message = "Maximum symlink depth exceeded",
                .sshError = SSH_OK,
                .sftpError = SSH_FX_FAILURE,
            }
        ));
    }

    std::expected<SftpSession::DeepLinkResult, SftpSession::Error>
    SftpSession::readLinkDeepInStrand(Paths::RemotePath const& path, int maxDepth)
    {
        return runInStrand(RemoteIo::ReadLinkDeep{.path = path, .maxDepth = maxDepth});
    }

    std::future<std::expected<SftpSession::DeepLinkResult, SftpSession::Error>>
    SftpSession::readLinkDeep(Paths::RemotePath const& path, int maxDepth)
    {
        return later(RemoteIo::ReadLinkDeep{.path = path, .maxDepth = maxDepth});
    }

    RemoteIo::OperationResult<RemoteIo::ReadLink> SftpSession::run(RemoteIo::ReadLink const& operation)
    {
        const std::unique_ptr<char, decltype(&ssh_string_free_char)> literal{
            sftp_readlink(session_.get(), operation.path.c_str()), ssh_string_free_char
        };
        if (!literal)
            return std::unexpected(lastError());
        return Paths::sanitizedUtf8(literal.get());
    }

    std::expected<std::string, SftpSession::Error> SftpSession::readLinkInStrand(Paths::RemotePath const& path)
    {
        return runInStrand(RemoteIo::ReadLink{.path = path});
    }

    RemoteIo::OperationResult<RemoteIo::CreateSymLink> SftpSession::run(RemoteIo::CreateSymLink const& operation)
    {
        const auto result = sftp_symlink(session_.get(), operation.target.c_str(), operation.linkPath.c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createSymLinkInStrand(std::string const& target, Paths::RemotePath const& linkPath)
    {
        return runInStrand(RemoteIo::CreateSymLink{.target = target, .linkPath = linkPath});
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createSymLink(std::string const& target, Paths::RemotePath const& linkPath)
    {
        return later(RemoteIo::CreateSymLink{.target = target, .linkPath = linkPath});
    }
}