#pragma once

#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/remote_file_system.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RemoteIo::Test
{
    class FakeRemoteFileSystem;

    /**
     * @brief A file of a FakeRemoteFileSystem opened for reading or writing.
     */
    class FakeRemoteFileStream final : public IRemoteFileStream
    {
      public:
        FakeRemoteFileStream(
            std::weak_ptr<FakeRemoteFileSystem> fileSystem,
            Paths::RemotePath path,
            OpenMode mode,
            std::size_t readLengthLimit
        )
            : fileSystem_{std::move(fileSystem)}
            , path_{std::move(path)}
            , mode_{mode}
            , readLengthLimit_{readLengthLimit}
        {}

        std::expected<void, RemoteError> seekInStrand(std::size_t position) override
        {
            position_ = position;
            return {};
        }
        std::expected<FileInformation, RemoteError> statInStrand() override;
        std::expected<std::size_t, RemoteError> readSomeInStrand(char* buffer, std::size_t bufferSize) override;
        std::future<std::expected<std::size_t, RemoteError>> readAll(std::function<bool(std::string_view)>) override
        {
            std::promise<std::expected<std::size_t, RemoteError>> unsupported;
            unsupported.set_value(std::unexpected(RemoteError{.kind = RemoteErrorKind::Unsupported}));
            return unsupported.get_future();
        }
        std::expected<void, RemoteError> writeInStrand(std::string_view data) override;
        std::expected<std::shared_ptr<AsyncTransferContext>, RemoteError>
        readAsyncInStrand(SignedSizeType, char*, SignedSizeType, std::function<bool(SignedSizeType)>) override
        {
            return std::unexpected(RemoteError{.kind = RemoteErrorKind::Unsupported});
        }
        std::expected<std::shared_ptr<AsyncTransferContext>, RemoteError> writeAsyncInStrand(
            SignedSizeType,
            char*,
            SignedSizeType,
            std::function<SignedSizeType(SignedSizeType)>
        ) override
        {
            return std::unexpected(RemoteError{.kind = RemoteErrorKind::Unsupported});
        }
        std::expected<void, RemoteError> finishInStrand() override
        {
            return {};
        }
        std::size_t readLengthLimit() const override
        {
            return readLengthLimit_;
        }
        std::size_t writeLengthLimit() const override
        {
            return 1024u;
        }
        void close() override;
        void closeInStrand() override;
        ProcessingStrand* strand() const override;

      private:
        std::weak_ptr<FakeRemoteFileSystem> fileSystem_;
        Paths::RemotePath path_;
        OpenMode mode_;
        std::size_t readLengthLimit_;
        std::size_t position_{0};
    };

    /**
     * @brief A remote file system in memory for tests. Completes every operation when it starts, counts the operations
     *        it started and fails those a test tells it to with @ref failWhen, or opens while its connections are
     *        taken, see @ref takeAllConnections. Runs the operations the helpers use, any other is Unsupported until a
     *        test needs it.
     */
    class FakeRemoteFileSystem final : public RemoteFileSystemImplementation<FakeRemoteFileSystem>
    {
      public:
        struct Options
        {
            Capabilities capabilities{};
            /** @brief Whether a rename replaces an existing destination when the capabilities say Unknown. */
            bool unknownRenameReplaces{true};
            /** @brief Smaller than the files of most tests, so they take several reads. */
            std::size_t readLengthLimit{1024u};
        };

        FakeRemoteFileSystem(ProcessingStrand* strand, Options options)
            : strand_{strand}
            , options_{std::move(options)}
        {
            nodes_.emplace("/", Node{.type = SharedData::FileType::Directory});
        }

        /**
         * @brief Adds a directory and its missing parents.
         */
        void addDirectory(Paths::RemotePath const& path)
        {
            std::scoped_lock lock{mutex_};
            addParentsOf(path);
            nodes_.insert_or_assign(path.value(), Node{.type = SharedData::FileType::Directory});
        }

        /**
         * @brief Adds a file and the missing directories above it.
         */
        void addFile(Paths::RemotePath const& path, std::string content)
        {
            std::scoped_lock lock{mutex_};
            addParentsOf(path);
            nodes_.insert_or_assign(
                path.value(), Node{.type = SharedData::FileType::Regular, .content = std::move(content)}
            );
        }

        /**
         * @brief The content of the file at @p path, nothing when there is no file.
         */
        std::optional<std::string> contentOf(Paths::RemotePath const& path) const
        {
            std::scoped_lock lock{mutex_};
            const auto node = nodes_.find(path.value());
            if (node == nodes_.end() || node->second.type != SharedData::FileType::Regular)
                return std::nullopt;
            return node->second.content;
        }

        bool exists(Paths::RemotePath const& path) const
        {
            std::scoped_lock lock{mutex_};
            return nodes_.contains(path.value());
        }

        std::size_t openStreams() const
        {
            std::scoped_lock lock{mutex_};
            return streams_.size();
        }

        /**
         * @brief Fails every later @p OperationT that @p matches with @p error, leaving the files as they are.
         */
        template <typename OperationT>
        void failWhen(std::function<bool(OperationT const&)> matches, RemoteError error)
        {
            std::scoped_lock lock{mutex_};
            failures_[RemoteFileSystemOperations::indexOf<OperationT>()].push_back(
                [matches = std::move(matches),
                    error = std::move(error)](void const* operation) -> std::optional<RemoteError>
                {
                    if (matches(*static_cast<OperationT const*>(operation)))
                        return error;
                    return std::nullopt;
                }
            );
        }

        /**
         * @brief Fails every open with Busy from now on, as a protocol whose connections all transfer, and keeps the
         *        wakes of those opens for @ref freeConnections.
         */
        void takeAllConnections()
        {
            takeConnectionsFor(
                [](OpenFile const&)
                {
                    return true;
                }
            );
        }

        /**
         * @brief Fails the opens that @p opens matches with Busy from now on, as if every connection were taken for
         *        them, and keeps their wakes for @ref freeConnections.
         */
        void takeConnectionsFor(std::function<bool(OpenFile const&)> opens)
        {
            std::scoped_lock lock{mutex_};
            takenFor_ = std::move(opens);
        }

        /**
         * @brief Lets opens through again and calls the wakes the Busy opens left, outside the lock.
         */
        void freeConnections()
        {
            std::vector<std::function<void()>> wakes;
            {
                std::scoped_lock lock{mutex_};
                takenFor_ = {};
                wakes = std::exchange(wakesWhenFree_, {});
            }
            for (auto const& wake : wakes)
                wake();
        }

        /**
         * @brief How many opens failed with Busy.
         */
        std::size_t busyOpens() const
        {
            std::scoped_lock lock{mutex_};
            return busyOpens_;
        }

        /**
         * @brief How many reads the streams of the fake took, each a round trip on a real server.
         */
        std::size_t streamReads() const
        {
            std::scoped_lock lock{mutex_};
            return streamReads_;
        }

        /**
         * @brief How many opens of @p path failed with Busy.
         */
        std::size_t busyOpensOf(Paths::RemotePath const& path) const
        {
            std::scoped_lock lock{mutex_};
            const auto count = busyOpensByPath_.find(path.value());
            return count == busyOpensByPath_.end() ? 0u : count->second;
        }

        /**
         * @brief How many @p OperationT were started.
         */
        template <typename OperationT>
        std::size_t started() const
        {
            std::scoped_lock lock{mutex_};
            return started_[RemoteFileSystemOperations::indexOf<OperationT>()];
        }

        /**
         * @brief Completes @p pending outside the lock, so a completion may use the fake again.
         */
        template <typename OperationT>
        void startOperation(Pending<OperationT> pending)
        {
            pending.complete(resultOf(pending.operation));
        }

        Capabilities capabilities() const override
        {
            return options_.capabilities;
        }
        ProcessingStrand* strand() const override
        {
            return strand_;
        }
        IBufferProvider& bufferProvider() override
        {
            return *bufferProvider_;
        }
        bool close() override
        {
            std::scoped_lock lock{mutex_};
            streams_.clear();
            return !std::exchange(closed_, true);
        }

      protected:
        void dispatch(std::function<void()> start) override
        {
            strand_->pushTask(std::move(start));
        }

      private:
        friend class FakeRemoteFileStream;

        /**
         * @brief The error to fail an operation of one type with, nothing to let it run.
         */
        using FailureRule = std::function<std::optional<RemoteError>(void const* operation)>;

        struct Node
        {
            SharedData::FileType type{SharedData::FileType::Regular};
            std::string content{};
        };

        static RemoteError errorOf(RemoteErrorKind kind, std::string message)
        {
            return RemoteError{.kind = kind, .message = std::move(message)};
        }
        static RemoteError noSuchFile(Paths::RemotePath const& path)
        {
            return errorOf(RemoteErrorKind::NoSuchFile, fmt::format("'{}' does not exist", path.value()));
        }

        void addParentsOf(Paths::RemotePath const& path)
        {
            for (auto parent = path.parent(); parent && !nodes_.contains(parent->value()); parent = parent->parent())
                nodes_.emplace(parent->value(), Node{.type = SharedData::FileType::Directory});
        }

        template <typename OperationT>
        OperationResult<OperationT> resultOf(OperationT const& operation)
        {
            std::scoped_lock lock{mutex_};
            constexpr auto index = RemoteFileSystemOperations::indexOf<OperationT>();
            ++started_[index];
            for (auto const& failure : failures_[index])
            {
                if (auto error = failure(&operation))
                    return std::unexpected(std::move(*error));
            }
            if constexpr (requires { run(operation); })
                return run(operation);
            else
                return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "Not in the fake"));
        }

        Node* find(Paths::RemotePath const& path)
        {
            const auto node = nodes_.find(path.value());
            return node == nodes_.end() ? nullptr : &node->second;
        }

        bool parentIsDirectory(Paths::RemotePath const& path)
        {
            const auto parent = path.parent();
            auto const* node = parent ? find(*parent) : nullptr;
            return node != nullptr && node->type == SharedData::FileType::Directory;
        }

        /**
         * @brief Moves @p source and everything below it to @p destination.
         */
        void move(Paths::RemotePath const& source, Paths::RemotePath const& destination)
        {
            std::vector<std::pair<std::string, Node>> moved;
            for (auto entry = nodes_.begin(); entry != nodes_.end();)
            {
                if (Paths::RemotePath::make(entry->first).value().isWithin(source))
                {
                    moved.emplace_back(destination.value() + entry->first.substr(source.value().size()), entry->second);
                    entry = nodes_.erase(entry);
                }
                else
                    ++entry;
            }
            for (auto& [path, node] : moved)
                nodes_.insert_or_assign(std::move(path), std::move(node));
        }

        OperationResult<Lstat> run(Lstat const& operation)
        {
            auto const* node = find(operation.path);
            if (node == nullptr)
                return std::unexpected(noSuchFile(operation.path));
            const auto name = operation.path.filename();
            return FileInformation{
                .name = name ? std::string{name->view()} : std::string{},
                .type = node->type,
                .size = node->content.size(),
            };
        }
        /**
         * @brief Like Lstat, the fake has no links.
         */
        OperationResult<Stat> run(Stat const& operation)
        {
            return run(Lstat{.path = operation.path});
        }
        OperationResult<ListDirectory> run(ListDirectory const& operation)
        {
            auto const* directory = find(operation.path);
            if (directory == nullptr)
                return std::unexpected(noSuchFile(operation.path));
            if (directory->type != SharedData::FileType::Directory)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "Not a directory"));
            std::vector<FileInformation> entries;
            for (auto const& [path, node] : nodes_)
            {
                const auto entry = Paths::RemotePath::make(path).value();
                if (entry.parent() != operation.path)
                    continue;
                const auto name = entry.filename();
                entries.push_back(
                    FileInformation{
                        .name = name ? std::string{name->view()} : std::string{},
                        .type = node.type,
                        .size = node.content.size(),
                    }
                );
            }
            return entries;
        }
        OperationResult<RemoveFile> run(RemoveFile const& operation)
        {
            auto const* node = find(operation.path);
            if (node == nullptr)
                return std::unexpected(noSuchFile(operation.path));
            if (node->type == SharedData::FileType::Directory)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "Is a directory"));
            nodes_.erase(operation.path.value());
            return {};
        }
        OperationResult<Rename> run(Rename const& operation)
        {
            if (find(operation.source) == nullptr)
                return std::unexpected(noSuchFile(operation.source));
            if (!parentIsDirectory(operation.destination))
                return std::unexpected(noSuchFile(operation.destination));
            if (operation.source == operation.destination)
                return {};
            if (auto const* destination = find(operation.destination); destination != nullptr)
            {
                const auto semantics = options_.capabilities.renameSemantics;
                const auto replaces = semantics == RenameSemantics::Replaces ||
                    (semantics == RenameSemantics::Unknown && options_.unknownRenameReplaces);
                // Plain SFTP and FTP servers refuse with a generic failure, not with AlreadyExists.
                if (!replaces)
                    return std::unexpected(errorOf(RemoteErrorKind::Failure, "The destination exists"));
                if (destination->type == SharedData::FileType::Directory)
                    return std::unexpected(errorOf(RemoteErrorKind::Failure, "The destination is a directory"));
                nodes_.erase(operation.destination.value());
            }
            move(operation.source, operation.destination);
            return {};
        }
        OperationResult<LinkIntoPlace> run(LinkIntoPlace const& operation)
        {
            auto const* source = find(operation.source);
            if (source == nullptr)
                return std::unexpected(noSuchFile(operation.source));
            if (find(operation.destination) != nullptr)
                return std::unexpected(errorOf(RemoteErrorKind::AlreadyExists, "The destination exists"));
            if (!options_.capabilities.hardLinks)
                return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "No hard links"));
            if (source->type != SharedData::FileType::Regular)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "Not a regular file"));
            if (!parentIsDirectory(operation.destination))
                return std::unexpected(noSuchFile(operation.destination));
            move(operation.source, operation.destination);
            return {};
        }
        OperationResult<CreateDirectoryIfItDoesntExist> run(CreateDirectoryIfItDoesntExist const& operation)
        {
            if (auto const* node = find(operation.path); node != nullptr)
            {
                if (node->type == SharedData::FileType::Directory)
                    return {};
                return std::unexpected(errorOf(RemoteErrorKind::AlreadyExists, "Not a directory"));
            }
            if (!parentIsDirectory(operation.path))
                return std::unexpected(noSuchFile(operation.path));
            nodes_.emplace(operation.path.value(), Node{.type = SharedData::FileType::Directory});
            return {};
        }
        OperationResult<OpenFile> run(OpenFile const& operation)
        {
            if (contains(operation.mode, OpenMode::Exclusive) && !options_.capabilities.exclusiveCreate)
                return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "No exclusive create"));
            if (takenFor_ && takenFor_(operation))
            {
                ++busyOpens_;
                ++busyOpensByPath_[operation.path.value()];
                if (operation.wakeWhenFree)
                    wakesWhenFree_.push_back(operation.wakeWhenFree);
                return std::unexpected(errorOf(RemoteErrorKind::Busy, "Every connection is taken"));
            }
            auto* node = find(operation.path);
            if (contains(operation.mode, OpenMode::Write))
            {
                if (node != nullptr && contains(operation.mode, OpenMode::Exclusive))
                    return std::unexpected(errorOf(RemoteErrorKind::AlreadyExists, "The file exists"));
                if (node == nullptr && !contains(operation.mode, OpenMode::Create))
                    return std::unexpected(noSuchFile(operation.path));
                if (node == nullptr)
                {
                    if (!parentIsDirectory(operation.path))
                        return std::unexpected(noSuchFile(operation.path));
                    node = &nodes_.emplace(operation.path.value(), Node{}).first->second;
                }
                if (contains(operation.mode, OpenMode::Truncate))
                    node->content.clear();
            }
            if (node == nullptr)
                return std::unexpected(noSuchFile(operation.path));
            if (node->type != SharedData::FileType::Regular)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "Not a regular file"));
            auto stream = std::make_shared<FakeRemoteFileStream>(
                weak_from_this(), operation.path, operation.mode, options_.readLengthLimit
            );
            streams_.push_back(stream);
            return stream;
        }
        OperationResult<ReadTransferLimits> run(ReadTransferLimits const&)
        {
            return TransferLimits{};
        }
        OperationResult<ReadFile> run(ReadFile const& operation)
        {
            auto const* node = find(operation.path);
            if (node == nullptr)
                return std::unexpected(noSuchFile(operation.path));
            if (node->type != SharedData::FileType::Regular)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "Not a regular file"));
            return node->content.substr(0, operation.maximumSize);
        }

        ProcessingStrand* strand_;
        Options options_;
        std::shared_ptr<IBufferProvider> bufferProvider_{DefaultBufferProvider::create()};
        mutable std::mutex mutex_{};
        std::map<std::string, Node> nodes_{};
        std::vector<std::shared_ptr<FakeRemoteFileStream>> streams_{};
        std::array<std::size_t, RemoteFileSystemOperations::size> started_{};
        std::array<std::vector<FailureRule>, RemoteFileSystemOperations::size> failures_{};
        std::function<bool(OpenFile const&)> takenFor_{};
        std::size_t streamReads_{0};
        std::size_t busyOpens_{0};
        std::map<std::string, std::size_t> busyOpensByPath_{};
        std::vector<std::function<void()>> wakesWhenFree_{};
        bool closed_{false};
    };

    inline std::expected<FileInformation, RemoteError> FakeRemoteFileStream::statInStrand()
    {
        const auto fileSystem = fileSystem_.lock();
        if (!fileSystem)
            return std::unexpected(RemoteError{.wrapperError = WrapperErrors::SharedPtrDestroyed});
        std::scoped_lock lock{fileSystem->mutex_};
        return fileSystem->run(Lstat{.path = path_});
    }

    inline std::expected<std::size_t, RemoteError>
    FakeRemoteFileStream::readSomeInStrand(char* buffer, std::size_t bufferSize)
    {
        const auto fileSystem = fileSystem_.lock();
        if (!fileSystem)
            return std::unexpected(RemoteError{.wrapperError = WrapperErrors::SharedPtrDestroyed});
        std::scoped_lock lock{fileSystem->mutex_};
        ++fileSystem->streamReads_;
        auto const* node = fileSystem->find(path_);
        if (node == nullptr)
            return std::unexpected(FakeRemoteFileSystem::noSuchFile(path_));
        if (position_ >= node->content.size())
            return 0u;
        const auto count = std::min(bufferSize, node->content.size() - position_);
        std::copy_n(node->content.begin() + static_cast<std::ptrdiff_t>(position_), count, buffer);
        position_ += count;
        return count;
    }

    inline std::expected<void, RemoteError> FakeRemoteFileStream::writeInStrand(std::string_view data)
    {
        if (!contains(mode_, OpenMode::Write))
            return std::unexpected(RemoteError{.kind = RemoteErrorKind::PermissionDenied, .message = "Opened to read"});
        const auto fileSystem = fileSystem_.lock();
        if (!fileSystem)
            return std::unexpected(RemoteError{.wrapperError = WrapperErrors::SharedPtrDestroyed});
        std::scoped_lock lock{fileSystem->mutex_};
        auto* node = fileSystem->find(path_);
        if (node == nullptr)
            return std::unexpected(FakeRemoteFileSystem::noSuchFile(path_));
        if (node->content.size() < position_ + data.size())
            node->content.resize(position_ + data.size());
        std::ranges::copy(data, node->content.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += data.size();
        return {};
    }

    inline void FakeRemoteFileStream::close()
    {
        const auto fileSystem = fileSystem_.lock();
        if (!fileSystem)
            return;
        if (fileSystem->strand()->withinProcessingThread())
            return closeInStrand();
        fileSystem
            ->performPromise(
                [this]()
                {
                    closeInStrand();
                    return true;
                }
            )
            .wait();
    }

    inline void FakeRemoteFileStream::closeInStrand()
    {
        const auto fileSystem = fileSystem_.lock();
        if (!fileSystem)
            return;
        std::scoped_lock lock{fileSystem->mutex_};
        std::erase_if(
            fileSystem->streams_,
            [this](std::shared_ptr<FakeRemoteFileStream> const& stream)
            {
                return stream.get() == this;
            }
        );
    }

    inline ProcessingStrand* FakeRemoteFileStream::strand() const
    {
        const auto fileSystem = fileSystem_.lock();
        return fileSystem ? fileSystem->strand() : nullptr;
    }
}
