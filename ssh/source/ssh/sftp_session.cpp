#include <ssh/sftp_session.hpp>
#include <ssh/session.hpp>
#include <ssh/file_information.hpp>

#include <cassert>
#include <fcntl.h>

namespace SecureShell
{
    SftpSession::SftpSession(Session* owner, std::unique_ptr<ProcessingStrand> strand, sftp_session session)
        : owner_{owner}
        , strand_{std::move(strand)}
        , session_{session}
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
    SftpSession::listDirectoryInStrand(std::filesystem::path const& path)
    {
        assert(strand_->withinProcessingThread());

        int closeResult = 0;
        std::vector<FileInformation> entries{};

        {
            std::unique_ptr<sftp_dir_struct, std::function<void(sftp_dir_struct*)>> dir{
                sftp_opendir(session_, path.generic_string().c_str()),
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
                const auto fullPath = (path / entry.path).generic_string();

                std::unique_ptr<char, decltype(&ssh_string_free_char)> linkLiteral{
                    sftp_readlink(session_, fullPath.c_str()), ssh_string_free_char
                };
                if (linkLiteral)
                    entry.linkTarget = std::filesystem::path{static_cast<const char*>(linkLiteral.get())};

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
    SftpSession::listDirectory(std::filesystem::path const& path)
    {
        return performPromise([this, path]() { return listDirectoryInStrand(path); });
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createDirectoryInStrand(std::filesystem::path const& path, std::filesystem::perms permissions)
    {
        assert(strand_->withinProcessingThread());
        auto result = sftp_mkdir(
            session_,
            path.generic_string().c_str(),
            static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
        );
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createDirectory(std::filesystem::path const& path, std::filesystem::perms permissions)
    {
        return performPromise([this, path, permissions]() { return createDirectoryInStrand(path, permissions); });
    }

    std::expected<void, SftpSession::Error> SftpSession::createDirectoryIfItDoesntExistInStrand(
        std::filesystem::path const& path,
        std::filesystem::perms permissions
    )
    {
        assert(strand_->withinProcessingThread());
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_lstat(session_, path.generic_string().c_str()), sftp_attributes_free
        };
        if (!attributes)
        {
            auto result = sftp_mkdir(
                session_,
                path.generic_string().c_str(),
                static_cast<unsigned long>(permissions & std::filesystem::perms::mask)
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
    SftpSession::createDirectoryIfItDoesntExist(std::filesystem::path const& path, std::filesystem::perms permissions)
    {
        return performPromise(
            [this, path, permissions]() { return createDirectoryIfItDoesntExistInStrand(path, permissions); }
        );
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createFileInStrand(std::filesystem::path const& path, std::filesystem::perms permissions)
    {
        assert(strand_->withinProcessingThread());
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file_struct*)>> file{
            sftp_open(
                session_,
                path.generic_string().c_str(),
                O_CREAT,
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
    SftpSession::createFile(std::filesystem::path const& path, std::filesystem::perms permissions)
    {
        return performPromise([this, path, permissions]() { return createFileInStrand(path, permissions); });
    }

    std::expected<void, SftpSession::Error> SftpSession::removeFileInStrand(std::filesystem::path const& path)
    {
        assert(strand_->withinProcessingThread());
        auto result = sftp_unlink(session_, path.generic_string().c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeFile(std::filesystem::path const& path)
    {
        return performPromise([this, path]() { return removeFileInStrand(path); });
    }

    std::expected<void, SftpSession::Error> SftpSession::removeDirectoryInStrand(std::filesystem::path const& path)
    {
        assert(strand_->withinProcessingThread());
        auto result = sftp_rmdir(session_, path.generic_string().c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>> SftpSession::removeDirectory(std::filesystem::path const& path)
    {
        return performPromise([this, path]() { return removeDirectoryInStrand(path); });
    }

    std::future<std::expected<std::vector<std::filesystem::path>, SftpSession::Error>>
    SftpSession::filterOutEmptyDirectories(std::vector<std::filesystem::path> directories)
    {
        return performPromise(
            [this, directories = std::move(directories)]() -> std::expected<std::vector<std::filesystem::path>, Error>
            {
                int closeResult = 0;
                std::vector<std::filesystem::path> nonEmpties;
                for (const auto& path : directories)
                {
                    std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                        sftp_lstat(session_, path.generic_string().c_str()), sftp_attributes_free
                    };

                    if (attributes == nullptr)
                        return std::unexpected(lastError());

                    if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
                    {
                        std::unique_ptr<sftp_dir_struct, std::function<void(sftp_dir_struct*)>> dir{
                            sftp_opendir(session_, path.generic_string().c_str()),
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
    SftpSession::removeAll(std::vector<std::filesystem::path> paths)
    {
        return performPromise(
            [this, paths = std::move(paths)]() -> std::expected<void, Error>
            {
                for (const auto& path : paths)
                {
                    std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                        sftp_lstat(session_, path.generic_string().c_str()), sftp_attributes_free
                    };

                    if (attributes == nullptr)
                        return std::unexpected(lastError());

                    if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
                    {
                        auto result = sftp_rmdir(session_, path.generic_string().c_str());
                        if (result != SSH_OK)
                            return std::unexpected(lastError());
                    }
                    else
                    {
                        auto result = sftp_unlink(session_, path.generic_string().c_str());
                        if (result != SSH_OK)
                            return std::unexpected(lastError());
                    }
                }
                return {};
            }
        );
    }

    std::expected<FileInformation, SftpSession::Error> SftpSession::statInStrand(std::filesystem::path const& path)
    {
        assert(strand_->withinProcessingThread());
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_stat(session_, path.generic_string().c_str()), sftp_attributes_free
        };
        if (attributes == nullptr)
            return std::unexpected(lastError());

        return fromSftpAttributes(attributes.get());
    }

    std::future<std::expected<FileInformation, SftpSession::Error>> SftpSession::stat(std::filesystem::path const& path)
    {
        return performPromise([this, path]() { return statInStrand(path); });
    }

    std::expected<FileInformation, SftpSession::Error> SftpSession::lstatInStrand(std::filesystem::path const& path)
    {
        assert(strand_->withinProcessingThread());
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_lstat(session_, path.generic_string().c_str()), sftp_attributes_free
        };
        if (attributes == nullptr)
            return std::unexpected(lastError());

        return fromSftpAttributes(attributes.get());
    }

    std::future<std::expected<FileInformation, SftpSession::Error>>
    SftpSession::lstat(std::filesystem::path const& path)
    {
        return performPromise([this, path]() { return lstatInStrand(path); });
    }

    std::expected<void, SftpSession::Error>
    SftpSession::statInStrand(std::filesystem::path const& path, sftp_attributes attributes)
    {
        assert(strand_->withinProcessingThread());
        auto result = sftp_setstat(session_, path.generic_string().c_str(), attributes);
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::stat(std::filesystem::path const& path, sftp_attributes attributes)
    {
        return performPromise([this, path, attributes]() { return statInStrand(path, attributes); });
    }

    std::expected<void, SftpSession::Error>
    SftpSession::renameInStrand(std::filesystem::path const& source, std::filesystem::path const& destination)
    {
        assert(strand_->withinProcessingThread());
        auto s = source.generic_string();
        auto d = destination.generic_string();

        auto result = sftp_rename(session_, s.c_str(), d.c_str());
        if (result != SSH_OK)
        {
            return std::unexpected(lastError());
        }
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::rename(std::filesystem::path const& source, std::filesystem::path const& destination)
    {
        return performPromise([this, source, destination]() { return renameInStrand(source, destination); });
    }

    std::expected<void, SftpSession::Error>
    SftpSession::chownInStrand(std::filesystem::path const& path, uid_t owner, gid_t group)
    {
        assert(strand_->withinProcessingThread());
        auto result = sftp_chown(session_, path.generic_string().c_str(), owner, group);
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::chown(std::filesystem::path const& path, uid_t owner, gid_t group)
    {
        return performPromise([this, path, owner, group]() { return chownInStrand(path, owner, group); });
    }

    std::expected<void, SftpSession::Error>
    SftpSession::chmodInStrand(std::filesystem::path const& path, std::filesystem::perms perms)
    {
        assert(strand_->withinProcessingThread());
        auto result = sftp_chmod(session_, path.generic_string().c_str(), static_cast<mode_t>(perms));
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::chmod(std::filesystem::path const& path, std::filesystem::perms perms)
    {
        return performPromise([this, path, perms]() { return chmodInStrand(path, perms); });
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
        return performPromise([this]() { return limitsInStrand(); });
    }

    std::expected<std::weak_ptr<FileStream>, SftpSession::Error>
    SftpSession::openFileInStrand(std::filesystem::path const& path, OpenType openType, std::filesystem::perms permissions)
    {
        assert(strand_->withinProcessingThread());
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file_struct*)>> file{
            sftp_open(
                session_,
                path.generic_string().c_str(),
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
    SftpSession::openFile(std::filesystem::path const& path, OpenType openType, std::filesystem::perms permissions)
    {
        return performPromise(
            [this, path, openType, permissions]() { return openFileInStrand(path, openType, permissions); }
        );
    }

    std::expected<SftpSession::DeepLinkResult, SftpSession::Error>
    SftpSession::readLinkDeepInStrand(std::filesystem::path const& path, int maxDepth)
    {
        assert(strand_->withinProcessingThread());
        std::filesystem::path currentPath = path;
        for (int depth = 0; depth < maxDepth; ++depth)
        {
            std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
                sftp_lstat(session_, currentPath.generic_string().c_str()), sftp_attributes_free
            };
            if (attributes == nullptr)
            {
                auto last = lastError();
                if (last.sftpError == 2)
                {
                    return DeepLinkResult{
                        .linkTarget = currentPath,
                        .targetInfo = std::nullopt,
                    };
                }
                return std::unexpected(std::move(last));
            }

            if (attributes->type != SSH_FILEXFER_TYPE_SYMLINK)
            {
                return DeepLinkResult{
                    .linkTarget = currentPath,
                    .targetInfo = fromSftpAttributes(attributes.get()),
                };
            }

            const auto readLinkResult = std::unique_ptr<char, decltype(&ssh_string_free_char)>(
                sftp_readlink(session_, currentPath.generic_string().c_str()), ssh_string_free_char
            );
            if (readLinkResult == nullptr)
                return std::unexpected(lastError());

            currentPath = readLinkResult.get();
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
    SftpSession::readLinkDeep(std::filesystem::path const& path, int maxDepth)
    {
        return performPromise([this, path, maxDepth]() { return readLinkDeepInStrand(path, maxDepth); });
    }

    std::expected<void, SftpSession::Error>
    SftpSession::createSymLinkInStrand(std::filesystem::path const& target, std::filesystem::path const& linkPath)
    {
        assert(strand_->withinProcessingThread());
        const auto result = sftp_symlink(session_, target.generic_string().c_str(), linkPath.generic_string().c_str());
        if (result != SSH_OK)
            return std::unexpected(lastError());
        return {};
    }

    std::future<std::expected<void, SftpSession::Error>>
    SftpSession::createSymLink(std::filesystem::path const& target, std::filesystem::path const& linkPath)
    {
        return performPromise([this, target, linkPath]() { return createSymLinkInStrand(target, linkPath); });
    }
}