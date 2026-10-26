#include <backend/sftp/local_copy_operation.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <system_error>

namespace
{
    std::filesystem::path withoutTrailingSeparator(std::filesystem::path const& path)
    {
        auto normal = path.lexically_normal();
        if (!normal.has_filename() && normal.has_relative_path())
            return normal.parent_path();
        return normal;
    }

    /**
     * @brief True when @p destination is the item @p source of @p kind reached by another path, a bind mount or a
     *        mapped drive. Links are never taken for their target. A file counts only when it lies in the same
     *        directory under the same name, a hard link elsewhere is another item of the same file.
     */
    bool isSameItem(
        std::filesystem::path const& source,
        Utility::NoFollowKind kind,
        std::filesystem::path const& destination
    )
    {
        if (kind == Utility::NoFollowKind::Link)
            return false;
        std::error_code error{};
        if (Utility::noFollowKindOf(destination, error) != kind || error)
            return false;
        if (kind == Utility::NoFollowKind::Directory)
            return std::filesystem::equivalent(source, destination, error) && !error;
        return source.filename() == destination.filename() &&
            std::filesystem::equivalent(source.parent_path(), destination.parent_path(), error) && !error;
    }

    /**
     * @brief Why the last stream operation failed, as far as errno tells.
     */
    std::string streamFailure()
    {
        return Utility::errorMessage(std::error_code{errno, std::generic_category()});
    }
}

std::optional<std::filesystem::path> LocalCopyOperation::freeCopyName(std::filesystem::path const& source)
{
    std::error_code error{};
    const auto isDirectory = Utility::noFollowKindOf(source, error) == Utility::NoFollowKind::Directory;
    if (error)
        return std::nullopt;
    const auto name = withoutTrailingSeparator(source).filename();
    const auto stem = isDirectory ? name : name.stem();
    const auto extension = isDirectory ? std::filesystem::path{} : name.extension();
    constexpr int maximumAttempts = 1000;
    for (int number = 1; number <= maximumAttempts; ++number)
    {
        auto candidateName = stem;
        candidateName += number == 1 ? std::string{" (copy)"} : fmt::format(" (copy {})", number);
        candidateName += extension;
        const auto candidate = withoutTrailingSeparator(source).parent_path() / candidateName;
        const auto kind = Utility::noFollowKindOf(candidate, error);
        if (error)
            return std::nullopt;
        if (kind == Utility::NoFollowKind::Missing)
            return candidate;
    }
    return std::nullopt;
}

bool LocalCopyOperation::isSameOrBelow(std::filesystem::path const& path, std::filesystem::path const& directory)
{
    const auto normalPath = withoutTrailingSeparator(path);
    const auto normalDirectory = withoutTrailingSeparator(directory);
    auto pathIterator = normalPath.begin();
    for (auto const& component : normalDirectory)
    {
        if (pathIterator == normalPath.end() || *pathIterator != component)
            return false;
        ++pathIterator;
    }
    return true;
}

LocalCopyOperation::LocalCopyOperation(LocalCopyOperationOptions options)
    : Operation{}
    , options_{std::move(options)}
{
    prepared_.reserve(options_.entries.size());
}

LocalCopyOperation::~LocalCopyOperation()
{
    discardFile();
}

std::expected<LocalCopyOperation::WorkStatus, LocalCopyOperation::Error> LocalCopyOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case NotStarted:
        case Preparing:
        case Prepared:
        {
            state_ = Running;
            [[fallthrough]];
        }
        case Running:
        {
            const auto deadline = std::chrono::steady_clock::now() + options_.stepBudget;
            do
            {
                switch (phase_)
                {
                    case Phase::Prepare:
                        workPrepare(deadline);
                        break;
                    case Phase::Scan:
                        workScan(deadline);
                        break;
                    case Phase::Copy:
                        workCopy(deadline);
                        break;
                    case Phase::RemoveSource:
                        workRemoveSource(deadline);
                        break;
                    case Phase::Done:
                        break;
                }
            } while (phase_ != Phase::Done && std::chrono::steady_clock::now() < deadline);

            reportProgress(phase_ == Phase::Done);
            if (phase_ != Phase::Done)
                return WorkStatus::MoreWork;
            if (failureCount_ > 0 && arrivedEntries_ == 0)
            {
                auto const& [path, error] = failedEntries_.front();
                return enterErrorState<WorkStatus>(Error{
                    .type = ErrorType::CopyFailed,
                    .extraInfo = fmt::format("{}: {}", path, error.toString()),
                });
            }
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case Finalizing:
        {
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case Completed:
        case PartialSuccess:
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        case Failed:
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        case Canceled:
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
    }
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

bool LocalCopyOperation::workPrepare(std::chrono::steady_clock::time_point)
{
    if (entryIndex_ < options_.entries.size())
        prepare(options_.entries[entryIndex_++]);
    if (entryIndex_ < options_.entries.size())
        return false;
    entryIndex_ = 0;
    phase_ = Phase::Scan;
    return true;
}

void LocalCopyOperation::prepare(SharedData::LocalCopyEntry const& entry)
{
    PreparedEntry prepared{
        .source = withoutTrailingSeparator(entry.source.native()),
        .destination = withoutTrailingSeparator(entry.destination.native()),
        .mayOverwrite = entry.mayOverwrite,
    };
    const auto settle = [this, &prepared]()
    {
        prepared.settled = true;
        prepared_.push_back(std::move(prepared));
    };
    const auto arrive = [this, &settle]()
    {
        ++arrivedEntries_;
        ++entriesDone_;
        ++countedEntries_;
        settle();
    };

    std::error_code error{};
    prepared.kind = Utility::noFollowKindOf(prepared.source, error);
    lastItem_ = Paths::nativeToUtf8(prepared.source);
    if (error)
    {
        recordFailure(prepared.source, ErrorType::FileStatFailed, error);
        return settle();
    }
    if (prepared.kind == Utility::NoFollowKind::Missing)
    {
        recordFailure(prepared.source, Error{.type = ErrorType::FileNotFound});
        return settle();
    }

    if (prepared.source == prepared.destination || isSameItem(prepared.source, prepared.kind, prepared.destination))
    {
        if (options_.move)
            return arrive();
        auto freeName = freeCopyName(prepared.source);
        if (!freeName)
        {
            recordFailure(prepared.source, Error{.type = ErrorType::FileExists, .extraInfo = "no free name is left"});
            return settle();
        }
        prepared.destination = *std::move(freeName);
    }
    else if (prepared.kind == Utility::NoFollowKind::Directory && isSameOrBelow(prepared.destination, prepared.source))
    {
        recordFailure(prepared.source, Error{.type = ErrorType::CopyIntoItself});
        return settle();
    }

    const auto destinationKind = Utility::noFollowKindOf(prepared.destination, error);
    if (error)
    {
        recordFailure(prepared.destination, ErrorType::FileStatFailed, error);
        return settle();
    }
    if (destinationKind != Utility::NoFollowKind::Missing)
    {
        if (!prepared.mayOverwrite)
        {
            recordFailure(prepared.destination, Error{.type = ErrorType::FileExists});
            return settle();
        }
        if ((prepared.kind == Utility::NoFollowKind::Directory) != (destinationKind == Utility::NoFollowKind::Directory))
        {
            recordFailure(
                prepared.destination,
                Error{
                    .type = ErrorType::OperationNotPossibleOnFileType,
                    .extraInfo = "a directory and a file cannot replace each other",
                }
            );
            return settle();
        }
    }

    // A directory is merged into an existing one by copying, everything else can simply be renamed.
    if (options_.move &&
        (destinationKind == Utility::NoFollowKind::Missing || prepared.kind != Utility::NoFollowKind::Directory))
    {
        std::filesystem::rename(prepared.source, prepared.destination, error);
        if (!error)
            return arrive();
        if (error != std::errc::cross_device_link)
        {
            recordFailure(prepared.source, ErrorType::RenameFailure, error);
            return settle();
        }
    }
    prepared_.push_back(std::move(prepared));
}

bool LocalCopyOperation::workScan(std::chrono::steady_clock::time_point deadline)
{
    if (entryIndex_ >= prepared_.size())
    {
        options_.scanProgress(countedBytes_, countedEntries_);
        entryIndex_ = 0;
        phase_ = Phase::Copy;
        return true;
    }
    auto& entry = prepared_[entryIndex_];
    if (entry.settled)
    {
        ++entryIndex_;
        return false;
    }
    if (!walk_)
        walk_.emplace(entry.source, Utility::NoFollowTreeWalk::Mode::Count);
    if (!walk_->step(deadline))
        return false;
    entry.entries = walk_->entries();
    entry.bytes = walk_->bytes();
    countedEntries_ += entry.entries;
    countedBytes_ += entry.bytes;
    walk_.reset();
    ++entryIndex_;
    return false;
}

bool LocalCopyOperation::workCopy(std::chrono::steady_clock::time_point deadline)
{
    if (entryIndex_ >= prepared_.size())
    {
        phase_ = Phase::Done;
        return true;
    }
    if (prepared_[entryIndex_].settled)
    {
        ++entryIndex_;
        return false;
    }
    if (!walkingEntry_)
    {
        startEntryCopy();
        return false;
    }
    if (file_)
        return continueFile(deadline);
    if (directories_.empty())
    {
        finishEntryCopy();
        return false;
    }

    auto& frame = directories_.back();
    if (frame.iterator == std::filesystem::directory_iterator{})
    {
        leaveDirectory();
        return false;
    }
    const auto source = frame.iterator->path();
    const auto destination = frame.destination / source.filename();
    std::error_code error{};
    frame.iterator.increment(error);
    if (error)
    {
        recordFailure(frame.source, ErrorType::DirectoryUnreadable, error);
        leaveDirectory();
        return false;
    }
    const auto kind = Utility::noFollowKindOf(source, error);
    if (error)
    {
        recordFailure(source, ErrorType::FileStatFailed, error);
        return false;
    }
    if (options_.move && renameItem(source, destination, kind))
        return false;
    copyItem(source, destination, kind, true);
    return false;
}

bool LocalCopyOperation::renameItem(
    std::filesystem::path const& source,
    std::filesystem::path const& destination,
    Utility::NoFollowKind kind
)
{
    using Utility::NoFollowKind;

    std::error_code error{};
    const auto destinationKind = Utility::noFollowKindOf(destination, error);
    if (error ||
        (destinationKind != NoFollowKind::Missing &&
         (kind == NoFollowKind::Directory || destinationKind == NoFollowKind::Directory)))
        return false;
    const auto size = kind == NoFollowKind::Other ? std::filesystem::file_size(source, error) : 0u;
    const auto bytes = error ? 0u : size;
    std::filesystem::rename(source, destination, error);
    if (error)
        return false;
    lastItem_ = Paths::nativeToUtf8(source);
    ++entriesDone_;
    bytesDone_ += bytes;
    return true;
}

bool LocalCopyOperation::workRemoveSource(std::chrono::steady_clock::time_point deadline)
{
    if (!walk_->step(deadline))
        return false;
    for (auto const& failure : walk_->failures())
        recordFailure(failure.path, ErrorType::DeleteFailed, failure.error);
    walk_.reset();
    phase_ = Phase::Copy;
    ++entryIndex_;
    return true;
}

void LocalCopyOperation::startEntryCopy()
{
    auto const& entry = prepared_[entryIndex_];
    walkingEntry_ = true;
    entryHadFailure_ = false;
    copyItem(entry.source, entry.destination, entry.kind, entry.mayOverwrite);
}

void LocalCopyOperation::finishEntryCopy()
{
    walkingEntry_ = false;
    if (entryHadFailure_)
    {
        if (options_.move)
            Log::warn(
                "LocalCopyOperation: '{}' is kept, not all of it could be moved.",
                Paths::nativeToUtf8(prepared_[entryIndex_].source)
            );
        ++entryIndex_;
        return;
    }
    ++arrivedEntries_;
    if (!options_.move)
    {
        ++entryIndex_;
        return;
    }
    walk_.emplace(prepared_[entryIndex_].source, Utility::NoFollowTreeWalk::Mode::Remove);
    phase_ = Phase::RemoveSource;
}

void LocalCopyOperation::copyItem(
    std::filesystem::path const& source,
    std::filesystem::path const& destination,
    Utility::NoFollowKind kind,
    bool replace
)
{
    using Utility::NoFollowKind;

    lastItem_ = Paths::nativeToUtf8(source);
    std::error_code error{};
    const auto destinationKind = Utility::noFollowKindOf(destination, error);
    if (error)
        return recordFailure(destination, ErrorType::FileStatFailed, error);
    if (destinationKind != NoFollowKind::Missing && !replace)
        return recordFailure(destination, Error{.type = ErrorType::FileExists});

    const auto refuseReplacing = [this, &destination]()
    {
        recordFailure(
            destination,
            Error{
                .type = ErrorType::OperationNotPossibleOnFileType,
                .extraInfo = "a directory and a file cannot replace each other",
            }
        );
    };

    switch (kind)
    {
        case NoFollowKind::Missing:
            return recordFailure(source, Error{.type = ErrorType::FileNotFound});
        case NoFollowKind::Link:
        {
            if (destinationKind == NoFollowKind::Directory)
                return refuseReplacing();
            if (destinationKind != NoFollowKind::Missing && !Utility::removeNoFollow(destination, error) && error)
                return recordFailure(destination, ErrorType::DeleteFailed, error);
            std::filesystem::copy_symlink(source, destination, error);
            if (error)
                return recordFailure(source, ErrorType::CannotCreateSymlink, error);
            ++entriesDone_;
            return;
        }
        case NoFollowKind::Directory:
        {
            if (destinationKind != NoFollowKind::Missing && destinationKind != NoFollowKind::Directory)
                return refuseReplacing();
            if (destinationKind == NoFollowKind::Missing && !std::filesystem::create_directory(destination, error) &&
                error)
                return recordFailure(destination, ErrorType::CannotCreateDirectory, error);
            std::filesystem::directory_iterator iterator{source, error};
            if (error)
                return recordFailure(source, ErrorType::DirectoryUnreadable, error);
            directories_.push_back({
                .source = source,
                .destination = destination,
                .iterator = std::move(iterator),
                .created = destinationKind == NoFollowKind::Missing,
            });
            return;
        }
        case NoFollowKind::Other:
        {
            if (destinationKind == NoFollowKind::Directory)
                return refuseReplacing();
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(source, error)) || error)
                return recordFailure(
                    source,
                    Error{
                        .type = ErrorType::OperationNotPossibleOnFileType,
                        .extraInfo = "only files, directories and links are copied",
                    }
                );
            return startFile(source, destination);
        }
    }
}

void LocalCopyOperation::startFile(std::filesystem::path const& source, std::filesystem::path const& destination)
{
    auto partialName = std::filesystem::path{"."};
    partialName += destination.filename();
    partialName += ".nui-sftp-copy";

    std::error_code error{};
    const auto size = std::filesystem::file_size(source, error);
    file_.emplace(FileCopy{
        .source = source,
        .destination = destination,
        .partial = destination.parent_path() / partialName,
        .total = error ? 0u : size,
    });
    file_->input.open(source, std::ios::binary);
    if (!file_->input)
    {
        recordFailure(source, Error{.type = ErrorType::OpenFailure, .extraInfo = streamFailure()});
        return discardFile();
    }
    file_->output.open(file_->partial, std::ios::binary | std::ios::trunc);
    if (!file_->output)
    {
        recordFailure(destination, Error{.type = ErrorType::OpenFailure, .extraInfo = streamFailure()});
        return discardFile();
    }
}

bool LocalCopyOperation::continueFile(std::chrono::steady_clock::time_point deadline)
{
    buffer_.resize(std::max<std::size_t>(options_.chunkSize, 1u));
    do
    {
        file_->input.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        const auto read = file_->input.gcount();
        if (read > 0)
        {
            file_->output.write(buffer_.data(), read);
            if (!file_->output)
            {
                recordFailure(file_->destination, Error{.type = ErrorType::CopyFailed, .extraInfo = streamFailure()});
                discardFile();
                return true;
            }
            file_->copied += static_cast<std::uint64_t>(read);
            bytesDone_ += static_cast<std::uint64_t>(read);
        }
        if (file_->input.eof())
        {
            finishFile();
            return true;
        }
        if (!file_->input)
        {
            recordFailure(file_->source, Error{.type = ErrorType::SourceFileNotGood, .extraInfo = streamFailure()});
            discardFile();
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void LocalCopyOperation::finishFile()
{
    file_->input.close();
    file_->output.close();
    if (!file_->output)
    {
        recordFailure(file_->destination, Error{.type = ErrorType::CopyFailed, .extraInfo = streamFailure()});
        return discardFile();
    }

    std::error_code error{};
    const auto status = std::filesystem::status(file_->source, error);
    if (!error)
        std::filesystem::permissions(file_->partial, status.permissions(), error);
    if (error)
        Log::warn(
            "LocalCopyOperation: The permissions of '{}' were not copied: {}",
            Paths::nativeToUtf8(file_->destination),
            Utility::errorMessage(error)
        );
    const auto modified = std::filesystem::last_write_time(file_->source, error);
    if (!error)
        std::filesystem::last_write_time(file_->partial, modified, error);
    if (error)
        Log::warn(
            "LocalCopyOperation: The modification time of '{}' was not copied: {}",
            Paths::nativeToUtf8(file_->destination),
            Utility::errorMessage(error)
        );

    std::filesystem::rename(file_->partial, file_->destination, error);
    if (error)
    {
        recordFailure(file_->destination, ErrorType::RenameFailure, error);
        return discardFile();
    }
    ++entriesDone_;
    file_.reset();
}

void LocalCopyOperation::discardFile()
{
    if (!file_)
        return;
    file_->input.close();
    file_->output.close();
    std::error_code error{};
    std::filesystem::remove(file_->partial, error);
    // The file is not copied, its bytes no longer count as done.
    bytesDone_ -= std::min(bytesDone_, file_->copied);
    file_.reset();
}

void LocalCopyOperation::leaveDirectory()
{
    const auto frame = std::move(directories_.back());
    directories_.pop_back();
    ++entriesDone_;
    // An existing directory that was merged into keeps its own permissions.
    if (!frame.created)
        return;
    // A read only source directory is copied as such, once its content is written.
    std::error_code error{};
    const auto status = std::filesystem::status(frame.source, error);
    if (!error)
        std::filesystem::permissions(frame.destination, status.permissions(), error);
    if (error)
        Log::warn(
            "LocalCopyOperation: The permissions of '{}' were not copied: {}",
            Paths::nativeToUtf8(frame.destination),
            Utility::errorMessage(error)
        );
}

void LocalCopyOperation::recordFailure(std::filesystem::path const& path, ErrorType type, std::error_code const& error)
{
    recordFailure(path, Error{.type = type, .extraInfo = Utility::errorMessage(error)});
}

void LocalCopyOperation::recordFailure(std::filesystem::path const& path, Error error)
{
    entryHadFailure_ = true;
    ++failureCount_;
    if (failedEntries_.size() >= maximumListedFailures)
        return;
    const auto pathText = Paths::nativeToUtf8(path);
    Log::error("LocalCopyOperation: '{}' failed: {}", pathText, error.toString());
    failedEntries_.emplace_back(pathText, std::move(error));
    if (failedEntries_.size() == maximumListedFailures)
        Log::error("LocalCopyOperation: Further failures are counted, not listed.");
}

void LocalCopyOperation::reportProgress(bool final)
{
    const auto now = std::chrono::steady_clock::now();
    if (!final && now - lastProgress_ < options_.progressInterval)
        return;
    lastProgress_ = now;
    if (phase_ == Phase::Scan)
    {
        const auto entries = countedEntries_ + (walk_ ? walk_->entries() : 0);
        const auto bytes = countedBytes_ + (walk_ ? walk_->bytes() : 0);
        return options_.scanProgress(bytes, entries);
    }
    options_.progress(Progress{
        .currentItem = lastItem_,
        .entriesDone = entriesDone_,
        .entriesTotal = std::max(countedEntries_, entriesDone_),
        .bytesDone = bytesDone_,
        .bytesTotal = std::max(countedBytes_, bytesDone_),
        .currentFile = file_ ? std::optional{Paths::nativeToUtf8(file_->destination)} : std::nullopt,
        .fileBytesDone = file_ ? file_->copied : 0u,
        .fileBytesTotal = file_ ? file_->total : 0u,
    });
}

std::expected<void, LocalCopyOperation::Error> LocalCopyOperation::cancel(bool adoptCancelState)
{
    discardFile();
    directories_.clear();
    walk_.reset();
    if (adoptCancelState)
        state_ = OperationState::Canceled;
    return {};
}
