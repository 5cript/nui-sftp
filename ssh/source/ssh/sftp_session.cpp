#include <ssh/sftp_session.hpp>
#include <ssh/session.hpp>
#include <ssh/file_information.hpp>
#include <paths/relative_path.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>

#include <fcntl.h>

namespace SecureShell
{
    SftpSession::SftpSession(Session* owner, std::unique_ptr<ProcessingStrand> strand, sftp_session session)
        : owner_{owner}
        , strand_{std::move(strand)}
        , session_{session}
        , bufferProvider_{DefaultBufferProvider::create()}
        , fileStreams_{}
    {}
    SftpSession::~SftpSession()
    {
        /* close makes no sense, since this lives in a shared_ptr and will only ever end here when it was already
         * removed */
    }
    bool SftpSession::close(bool isBackElement)
    {
        if (strand_->isFinalized())
            return false;

        return strand_
            ->pushFinalPromiseTask(
                [this, isBackElement]()
                {
                    removeAllFileStreams();
                    owner_->sftpSessionRemoveItself(this, isBackElement);
                    return true;
                }
            )
            .get();
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

    std::expected<std::vector<FileInformation>, SftpSession::Error>
    SftpSession::listDirectoryInStrand(Paths::RemotePath const& path)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());

        int closeResult = 0;
        std::vector<FileInformation> entries{};

        {
            std::unique_ptr<sftp_dir_struct, std::function<void(sftp_dir_struct*)>> dir{
                sftp_opendir(session_, path.c_str()),
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
                    sftp_readdir(session_, dir.get()), sftp_attributes_free
                };

                for (; entry != nullptr; entry.reset(sftp_readdir(session_, dir.get())))
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
            if (entry.type == SharedData::FileType::Symlink)
            {
                // A name that is no valid segment, like "x/..", must not become a path to look up.
                const auto name = entry.nameSegment();
                if (!name)
                    continue;
                const auto fullPath = path / *name;

                std::unique_ptr<char, decltype(&ssh_string_free_char)> linkLiteral{
                    sftp_readlink(session_, fullPath.c_str()), ssh_string_free_char
                };
                if (linkLiteral)
                {
                    const auto* raw = static_cast<const char*>(linkLiteral.get());
                    entry.linkTarget = std::string{raw ? raw : ""};
                }

                std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> targetAttrs{
                    sftp_stat(session_, fullPath.c_str()), sftp_attributes_free
                };
                if (targetAttrs != nullptr)
                    entry.resolvedTarget =
                        std::make_shared<SharedData::DirectoryEntry>(fromSftpAttributes(targetAttrs.get()));
            }
        }

        return entries;
    }

    std::future<std::expected<std::vector<FileInformation>, SftpSession::Error>>
    SftpSession::listDirectory(Paths::RemotePath const& path)
    {
        return performPromise(
            [this, path]()
            {
                return listDirectoryInStrand(path);
            }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createDirectoryInStrand(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        auto result = sftp_mkdir(
            session_, path.c_str(), static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
        );
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createDirectory(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return performPromise(
            [this, path, permissions]()
            {
                return createDirectoryInStrand(path, permissions);
            }
        );
    }

    std::expected<void, SftpSession::Error> SftpSession::createDirectoryIfItDoesntExistInStrand(
        Paths::RemotePath const& path,
        std::filesystem::perms permissions
    )
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_lstat(session_, path.c_str()), sftp_attributes_free
        };
        if (!attributes)
        {
            auto result = sftp_mkdir(
                session_, path.c_str(), static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
            );
            if (result != SSH_OK)
                return std::unexpected(lastError());
        }
        else
        {
            if (attributes->type != SSH_FILEXFER_TYPE_DIRECTORY)
            {
                return std::unexpected(
                    SftpSession::Error{
                        .message = "Path exists and is not a directory",
                        .sshError = SSH_OK,
                        .sftpError = SSH_FX_FILE_ALREADY_EXISTS,
                    }
                );
            }
        }
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createDirectoryIfItDoesntExist(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return performPromise(
            [this, path, permissions]()
            {
                return createDirectoryIfItDoesntExistInStrand(path, permissions);
            }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createFileInStrand(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file_struct*)>> file{
            // O_EXCL: an existing file is an error, not reported as created.
            sftp_open(
                session_,
                path.c_str(),
                O_CREAT | O_EXCL,
                static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
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

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createFile(Paths::RemotePath const& path, std::filesystem::perms permissions)
    {
        return performPromise(
            [this, path, permissions]()
            {
                return createFileInStrand(path, permissions);
            }
        );
    }

    std::expected<void, SftpSession::Error> SftpSession::removeFileInStrand(Paths::RemotePath const& path)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        auto result = sftp_unlink(session_, path.c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeFile(Paths::RemotePath const& path)
    {
        return performPromise(
            [this, path]()
            {
                return removeFileInStrand(path);
            }
        );
    }

    std::expected<void, SftpSession::Error> SftpSession::removeDirectoryInStrand(Paths::RemotePath const& path)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        auto result = sftp_rmdir(session_, path.c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeDirectory(Paths::RemotePath const& path)
    {
        return performPromise(
            [this, path]()
            {
                return removeDirectoryInStrand(path);
            }
        );
    }

    std::future<std::expected<std::vector<Paths::RemotePath>, SftpSession::Error>>
    SftpSession::filterOutEmptyDirectories(std::vector<Paths::RemotePath> directories)
    {
        return performPromise(
            [this, directories = std::move(directories)]() -> std::expected<std::vector<Paths::RemotePath>, Error>
            {
                int closeResult = 0;
                std::vector<Paths::RemotePath> nonEmpties;
                for (const auto& path : directories)
                {
                    if (auto valid = requireSetInStrand(path); !valid)
                        return std::unexpected(valid.error());
                    std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                        sftp_lstat(session_, path.c_str()), sftp_attributes_free
                    };

                    if (attributes == nullptr)
                        return std::unexpected(lastError());

                    if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
                    {
                        std::unique_ptr<sftp_dir_struct, std::function<void(sftp_dir_struct*)>> dir{
                            sftp_opendir(session_, path.c_str()),
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

                        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> entry{
                            sftp_readdir(session_, dir.get()), sftp_attributes_free
                        };
                        using namespace std::string_literals;

                        auto findNonDotEntry = [this, &entry, &dir]()
                        {
                            if (entry == nullptr)
                                return false;
                            if (entry->name == ".."s || entry->name == "."s)
                            {
                                entry.reset(sftp_readdir(session_, dir.get()));
                                return false;
                            }
                            else
                                return true;
                        };

                        auto result = findNonDotEntry();
                        if (!result)
                            result = findNonDotEntry();
                        if (!result)
                            result = findNonDotEntry();

                        if (result)
                            nonEmpties.push_back(path);
                    }
                }
                if (closeResult != SSH_OK)
                {
                    return std::unexpected(lastError());
                }
                return nonEmpties;
            }
        );
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::removeAll(std::vector<Paths::RemotePath> paths)
    {
        return performPromise(
            [this, paths = std::move(paths)]() -> std::expected<void, Error>
            {
                for (const auto& path : paths)
                {
                    if (auto valid = requireSetInStrand(path); !valid)
                        return std::unexpected(valid.error());
                    std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                        sftp_lstat(session_, path.c_str()), sftp_attributes_free
                    };

                    if (attributes == nullptr)
                        return std::unexpected(lastError());

                    if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
                    {
                        auto result = sftp_rmdir(session_, path.c_str());
                        if (result != SSH_OK)
                            return std::unexpected(lastError());
                    }
                    else
                    {
                        auto result = sftp_unlink(session_, path.c_str());
                        if (result != SSH_OK)
                            return std::unexpected(lastError());
                    }
                }
                return {};
            }
        );
    }

    std::expected<FileInformation, SftpSession::Error> SftpSession::statInStrand(Paths::RemotePath const& path)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const auto callStart = std::chrono::steady_clock::now();
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_stat(session_, path.c_str()), sftp_attributes_free
        };
        chunkController_.recordRoundTrip(std::chrono::steady_clock::now() - callStart);
        if (attributes == nullptr)
            return std::unexpected(lastError());

        return fromSftpAttributes(attributes.get());
    }

    std::future<std::expected<FileInformation, SftpSession::Error>> SftpSession::stat(Paths::RemotePath const& path)
    {
        return performPromise(
            [this, path]()
            {
                return statInStrand(path);
            }
        );
    }

    std::expected<FileInformation, SftpSession::Error> SftpSession::lstatInStrand(Paths::RemotePath const& path)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const auto callStart = std::chrono::steady_clock::now();
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_lstat(session_, path.c_str()), sftp_attributes_free
        };
        chunkController_.recordRoundTrip(std::chrono::steady_clock::now() - callStart);
        if (attributes == nullptr)
            return std::unexpected(lastError());

        return fromSftpAttributes(attributes.get());
    }

    std::future<std::expected<FileInformation, SftpSession::Error>>
    SftpSession::lstat(Paths::RemotePath const& path)
    {
        return performPromise(
            [this, path]()
            {
                return lstatInStrand(path);
            }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::statInStrand(Paths::RemotePath const& path, sftp_attributes attributes)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        auto result = sftp_setstat(session_, path.c_str(), attributes);
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

    std::expected<void, SftpSession::Error>
    SftpSession::renameInStrand(Paths::RemotePath const& source, Paths::RemotePath const& destination)
    {
        if (auto valid = requireSetInStrand(source, destination); !valid)
            return std::unexpected(valid.error());
        auto result = sftp_rename(session_, source.c_str(), destination.c_str());
        if (result != SSH_OK)
        {
            return std::unexpected(lastError());
        }
        return {};
    }

    bool SftpSession::supportsExtensionInStrand(char const* name, char const* version) const
    {
        assert(strand_->withinProcessingThread());
        return sftp_extension_supported(session_, name, version) == 1;
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

    std::expected<void, SftpSession::Error>
    SftpSession::hardlinkIntoPlaceInStrand(Paths::RemotePath const& source, Paths::RemotePath const& destination)
    {
        if (auto valid = requireSetInStrand(source, destination); !valid)
            return valid;

        if (sftp_hardlink(session_, source.c_str(), destination.c_str()) == SSH_OK)
        {
            if (auto removed = removeFileInStrand(source); !removed.has_value())
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

        const auto linkError = lastError();
        const auto existing = lstatInStrand(destination);
        if (existing.has_value())
            return std::unexpected(alreadyExists(destination));
        if (existing.error().sftpError != SSH_FX_NO_SUCH_FILE)
            return std::unexpected(linkError);

        // Filesystems without hard links, and Linux protected_hardlinks, refuse the link.
        Log::debug("SftpSession: Cannot link '{}': {}, renaming instead.", source.value(), linkError.toString());
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
            return std::unexpected(
                SftpError{
                    .message = fmt::format("Invalid suffix '{}' for '{}'", asideSuffix, destination.value()),
                    .wrapperError = WrapperErrors::InvalidPath,
                }
            );
        }

        const auto destinationInfo = lstatInStrand(destination);
        if (!destinationInfo.has_value())
        {
            if (destinationInfo.error().sftpError != SSH_FX_NO_SUCH_FILE || !lstatInStrand(*aside).has_value())
                return renameInStrand(source, destination);
            // A destination that only exists aside, from a rollback that failed, is put back first.
            if (auto restored = renameInStrand(*aside, destination); !restored.has_value())
                return restored;
        }
        else if (destinationInfo->isDirectory())
        {
            return std::unexpected(
                SftpError{
                    .message = fmt::format("'{}' is a directory", destination.value()),
                    .sftpError = SSH_FX_FAILURE,
                }
            );
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

    SftpError SftpSession::alreadyExists(Paths::RemotePath const& destination)
    {
        return SftpError{
            .message = fmt::format("'{}' already exists", destination.value()),
            .sftpError = SSH_FX_FILE_ALREADY_EXISTS,
        };
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::rename(Paths::RemotePath const& source, Paths::RemotePath const& destination)
    {
        return performPromise(
            [this, source, destination]()
            {
                return renameInStrand(source, destination);
            }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::chownInStrand(Paths::RemotePath const& path, uid_t owner, gid_t group)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        auto result = sftp_chown(session_, path.c_str(), owner, group);
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
        auto result = sftp_chmod(session_, path.c_str(), static_cast<mode_t>(perms));
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
            sftp_canonicalize_path(session_, path), ssh_string_free_char
        };
        if (canonical == nullptr)
            return std::unexpected(lastError());
        return Paths::RemotePath::make(canonical.get())
            .transform_error(
                [](Paths::PathError const& error)
                {
                    return Error{
                        .message = fmt::format("The server canonicalized to {}", error.toString()),
                        .sftpError = SSH_FX_FAILURE,
                        .wrapperError = WrapperErrors::InvalidPath,
                    };
                }
            );
    }

    std::expected<Paths::RemotePath, SftpSession::Error> SftpSession::canonicalHomeDirectoryInStrand()
    {
        return canonicalizeInStrand(".");
    }

    std::expected<Paths::RemotePath, SftpSession::Error>
    SftpSession::canonicalPathInStrand(Paths::RemotePath const& path)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        return canonicalizeInStrand(path.c_str());
    }

    std::expected<Paths::RemotePath, SftpSession::Error> SftpSession::homeDirectoryInStrand()
    {
        assert(strand_->withinProcessingThread());
        if (auto home = canonicalHomeDirectoryInStrand(); home)
            return home;

        char* rawUser = nullptr;
        const auto optionResult = ssh_options_get(session_->session, SSH_OPTIONS_USER, &rawUser);
        const std::unique_ptr<char, decltype(&ssh_string_free_char)> user{rawUser, ssh_string_free_char};
        if (optionResult != SSH_OK || user == nullptr || user.get()[0] == '\0')
            return std::unexpected(Error{.message = "Server did not report a home directory and the user is unknown"});

        const auto guess = Paths::RemotePath::make(fmt::format("/home/{}", user.get()));
        if (!guess)
            return Paths::RemotePath::root();
        const auto information = statInStrand(*guess);
        if (information && information->isDirectory())
            return *guess;
        return Paths::RemotePath::root();
    }

    std::future<std::expected<Paths::RemotePath, SftpSession::Error>> SftpSession::homeDirectory()
    {
        return performPromise(
            [this]()
            {
                return homeDirectoryInStrand();
            }
        );
    }

    std::expected<std::string, SftpSession::Error>
    SftpSession::readSmallFileInStrand(Paths::RemotePath const& path, std::size_t maximumSize)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const std::unique_ptr<sftp_file_struct, decltype(&sftp_close)> file{
            sftp_open(session_, path.c_str(), O_RDONLY, 0), sftp_close
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
                    if (error.sftpError == SSH_FX_NO_SUCH_FILE)
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

    SftpError SftpSession::unsetPathError()
    {
        return Error{
            .message = "The remote path is not set",
            .sftpError = SSH_FX_FAILURE,
            .wrapperError = WrapperErrors::InvalidPath,
        };
    }

    SftpError SftpSession::lastError() const
    {
        const auto result = SftpError{
            .message = "",
            .sshError = 0,
            .sftpError = sftp_get_error(session_),
        };
        return result;
    }

    std::expected<sftp_limits_struct, SftpSession::Error> SftpSession::limitsInStrand()
    {
        assert(strand_->withinProcessingThread());
        auto const* limits = sftp_limits(session_);
        if (limits == nullptr)
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

    std::expected<std::weak_ptr<FileStream>, SftpSession::Error> SftpSession::openFileInStrand(
        Paths::RemotePath const& path,
        OpenType openType,
        std::filesystem::perms permissions
    )
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        const auto callStart = std::chrono::steady_clock::now();
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file_struct*)>> file{
            sftp_open(
                session_,
                path.c_str(),
                static_cast<int>(openType),
                static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
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

        auto const* limits = sftp_limits(session_);
        if (limits == nullptr)
            return std::unexpected(lastError());

        auto stream = std::make_shared<FileStream>(shared_from_this(), file.release(), *limits);
        fileStreams_.push_back(stream);

        return std::weak_ptr<FileStream>{stream};
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

    std::expected<SftpSession::DeepLinkResult, SftpSession::Error>
    SftpSession::readLinkDeepInStrand(Paths::RemotePath const& path, int maxDepth)
    {
        if (auto valid = requireSetInStrand(path); !valid)
            return std::unexpected(valid.error());
        auto currentPath = path;
        std::optional<std::string> linkContent{};
        for (int depth = 0; depth < maxDepth; ++depth)
        {
            std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                sftp_lstat(session_, currentPath.c_str()), sftp_attributes_free
            };
            if (attributes == nullptr)
            {
                auto last = lastError();
                if (last.sftpError == SSH_FX_NO_SUCH_FILE)
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
                sftp_readlink(session_, currentPath.c_str()), ssh_string_free_char
            );
            if (readLinkResult == nullptr)
                return std::unexpected(lastError());
            if (!linkContent)
                linkContent = readLinkResult.get();

            // Relative content is relative to the link's directory, not to the server's working directory.
            auto next = currentPath.resolveLink(readLinkResult.get());
            if (!next)
            {
                return std::unexpected(
                    Error{
                        .message = fmt::format(
                            "Symlink '{}' has an unusable target: {}", currentPath.value(), next.error().toString()
                        ),
                        .sftpError = SSH_FX_FAILURE,
                        .wrapperError = WrapperErrors::InvalidPath,
                    }
                );
            }
            currentPath = *std::move(next);
        }

        return std::unexpected(
            Error{
                .message = "Maximum symlink depth exceeded",
                .sshError = SSH_OK,
                .sftpError = SSH_FX_FAILURE,
            }
        );
    }

    std::future<std::expected<SftpSession::DeepLinkResult, SftpSession::Error>>
    SftpSession::readLinkDeep(Paths::RemotePath const& path, int maxDepth)
    {
        return performPromise(
            [this, path, maxDepth]()
            {
                return readLinkDeepInStrand(path, maxDepth);
            }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createSymLinkInStrand(std::string const& target, Paths::RemotePath const& linkPath)
    {
        if (auto valid = requireSetInStrand(linkPath); !valid)
            return std::unexpected(valid.error());
        const auto result = sftp_symlink(session_, target.c_str(), linkPath.c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createSymLink(std::string const& target, Paths::RemotePath const& linkPath)
    {
        return performPromise(
            [this, target, linkPath]()
            {
                return createSymLinkInStrand(target, linkPath);
            }
        );
    }
}