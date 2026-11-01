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
#include <utility>
#include <vector>

namespace RemoteIo::Test
{
    class FakeRemoteFileSystem;

    /**
     * @brief A file of a FakeRemoteFileSystem opened for reading.
     */
    class FakeRemoteFileStream final : public IRemoteFileStream
    {
      public:
        FakeRemoteFileStream(
            std::weak_ptr<FakeRemoteFileSystem> fileSystem,
            Paths::RemotePath path,
            std::size_t readLengthLimit
        )
            : fileSystem_{std::move(fileSystem)}
            , path_{std::move(path)}
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
        std::expected<void, RemoteError> writeInStrand(std::string_view) override
        {
            return std::unexpected(RemoteError{.kind = RemoteErrorKind::Unsupported});
        }
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
        std::size_t readLengthLimit_;
        std::size_t position_{0};
    };

    /**
     * @brief A remote file system in memory for tests. Completes every operation when it starts, counts the operations
     *        it started and fails those a test tells it to with @ref failWhen. Runs the operations the helpers use,
     *        any other is Unsupported until a test needs it.
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
        OperationResult<OpenFile> run(OpenFile const& operation)
        {
            if (operation.mode != OpenMode::Read)
                return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "The fake only reads"));
            auto const* node = find(operation.path);
            if (node == nullptr)
                return std::unexpected(noSuchFile(operation.path));
            if (node->type != SharedData::FileType::Regular)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "Not a regular file"));
            auto stream =
                std::make_shared<FakeRemoteFileStream>(weak_from_this(), operation.path, options_.readLengthLimit);
            streams_.push_back(stream);
            return stream;
        }

        ProcessingStrand* strand_;
        Options options_;
        std::shared_ptr<IBufferProvider> bufferProvider_{DefaultBufferProvider::create()};
        mutable std::mutex mutex_{};
        std::map<std::string, Node> nodes_{};
        std::vector<std::shared_ptr<FakeRemoteFileStream>> streams_{};
        std::array<std::size_t, RemoteFileSystemOperations::size> started_{};
        std::array<std::vector<FailureRule>, RemoteFileSystemOperations::size> failures_{};
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
