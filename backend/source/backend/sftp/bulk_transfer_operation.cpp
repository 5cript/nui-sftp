#include <backend/sftp/bulk_transfer_operation.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>

#include <algorithm>

using namespace std::chrono_literals;

BulkTransferOperation::BulkTransferOperation(std::weak_ptr<SecureShell::SftpSession> sftp, CommonOptions common)
    : Operation{}
    , sftp_{std::move(sftp)}
    , common_{std::move(common)}
    , budget_{std::max(1, common_.concurrency)}
{}

BulkTransferOperation::~BulkTransferOperation()
{
    std::ignore = cancel(false);
    waitForStrandTasks(strand());
}

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error> BulkTransferOperation::work()
{
    if (scanPhase_)
    {
        auto result = stepScanPhase(scanPhase_);
        if (!result.has_value())
            return enterErrorState<WorkStatus>(std::move(result).error());
        return result;
    }

    const auto sftp = openSession(sftp_);
    if (!sftp)
        return enterErrorState<WorkStatus>(channelClosedError());
    auto fut = sftp->performPromise(
        [this]()
        {
            return workInStrand();
        }
    );
    if (fut.wait_for(common_.futureTimeout) != std::future_status::ready)
    {
        Log::error("{}: work umbrella timed out.", logName());
        return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error> BulkTransferOperation::workInStrand()
{
    using enum OperationState;

    if (scanPhase_)
    {
        Log::error("{}: The scan phase cannot run on the processing thread.", logName());
        return enterErrorState<WorkStatus>(
            Error{.type = ErrorType::ImplementationError, .extraInfo = "Scan phase stepped on the processing thread."}
        );
    }

    switch (state())
    {
        case (NotStarted):
        case (Preparing):
        case (Prepared):
        case (Running):
        {
            const auto sftp = openSession(sftp_);
            if (!sftp)
                return enterErrorState<WorkStatus>(channelClosedError());
            return state() == NotStarted ? start(*sftp) : step(*sftp);
        }
        case (Finalizing):
        {
            enterState(Completed);
            return WorkStatus::Complete;
        }
        case (Completed):
        {
            Log::warn("{}: Operation already completed.", logName());
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("{}: Operation already failed.", logName());
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("{}: Operation was canceled.", logName());
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("{}: Operation completed with partial success.", logName());
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    Log::error("{}: Unknown operation state: {}", logName(), static_cast<int>(state()));
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error>
BulkTransferOperation::start(SecureShell::SftpSession& sftp)
{
    if (entrySource_ == EntrySource::None)
    {
        Log::error("{}: Started without entries.", logName());
        return enterErrorState<WorkStatus>(
            Error{.type = ErrorType::ImplementationError, .extraInfo = "Neither a scan result nor a file list was set."}
        );
    }
    if (entries_.empty())
    {
        if (entrySource_ == EntrySource::Tree)
        {
            Log::error("{}: The scan delivered no entries.", logName());
            return enterErrorState<WorkStatus>(Error{.type = ErrorType::ScanIncomplete});
        }
        Log::info("{}: No entries to transfer.", logName());
        enterState(OperationState::Completed);
        return WorkStatus::Complete;
    }

    if (prescanned())
    {
        // Flat list of absolute (source, destination) pairs: no root to prepare, each child
        // creates its own missing parents.
        cursor_ = 0;
    }
    else
    {
        auto const& root = entries_[0];
        if (!root.isDirectory() || root.parent)
        {
            Log::error("{}: The first entry is not the root directory of the scan.", logName());
            return enterErrorState<WorkStatus>(
                Error{.type = ErrorType::ImplementationError, .extraInfo = "First entry must be a directory."}
            );
        }
        if (common_.remotePath.empty() || common_.localPath.empty())
        {
            Log::error("{}: A scanned tree needs a remote and a local root.", logName());
            return enterErrorState<WorkStatus>(
                Error{.type = ErrorType::ImplementationError, .extraInfo = "A tree needs both roots."}
            );
        }
        const auto result = prepareRootInStrand(sftp, root);
        if (!result.has_value())
            return enterErrorState<WorkStatus>(result.error());
        finishedEntries_[0] = true;
        cursor_ = 1;
    }

    enterState(OperationState::Running);
    emitProgress(true);
    return WorkStatus::MoreWork;
}

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error>
BulkTransferOperation::step(SecureShell::SftpSession& sftp)
{
    if (cursor_ > entries_.size())
    {
        Log::error("{}: Cursor out of range.", logName());
        return enterErrorState<WorkStatus>(Error{
            .type = ErrorType::ImplementationError,
            .extraInfo = "Bulk transfer cursor is beyond the item count, which should never occur."
        });
    }

    const auto slotsBefore = slots_.size();
    if (const auto filled = fillSlots(sftp); !filled.has_value())
        return enterErrorState<WorkStatus>(filled.error());
    // Report newly started files before stepping them: small files finish within this very
    // step and would otherwise never be seen in flight.
    if (slots_.size() > slotsBefore)
        emitProgress(true);

    if (const auto stepped = stepSlots(); !stepped.has_value())
        return enterErrorState<WorkStatus>(stepped.error());

    std::erase_if(
        slots_,
        [](Slot const& slot)
        {
            return slot.finished;
        }
    );

    if (slots_.empty() && deferred_.empty() && cursor_ == entries_.size())
    {
        if (const auto finished = finishInStrand(); !finished.has_value())
            return enterErrorState<WorkStatus>(finished.error());
        emitProgress(true);
        Log::info("{}: Bulk transfer completed.", logName());
        enterState(OperationState::Completed);
        return WorkStatus::Complete;
    }

    emitProgress(false);
    return WorkStatus::MoreWork;
}

std::expected<void, BulkTransferOperation::Error> BulkTransferOperation::fillSlots(SecureShell::SftpSession& sftp)
{
    // Directories are created inline and each blocks the strand (a round trip or two on
    // upload), so a step creates only a few before yielding to the queue.
    constexpr std::size_t directoriesPerStep = 8;
    std::size_t directoriesCreated = 0;
    while (static_cast<int>(slots_.size()) < budget_)
    {
        // While buffers are short, only probe with a deferred entry once nothing else runs;
        // fresh entries would just burn their prepare round trips and get deferred as well.
        std::size_t index = 0;
        if (!deferred_.empty() && (retryDeferred_ || slots_.empty()))
        {
            index = deferred_.front();
            deferred_.pop_front();
        }
        else if (cursor_ < entries_.size() && retryDeferred_)
        {
            if (entries_[cursor_].isDirectory() && directoriesCreated == directoriesPerStep)
                break;
            index = cursor_++;
        }
        else
        {
            break;
        }

        auto const& entry = entries_[index];
        const auto paths = entryPaths(index);
        if (!paths)
        {
            Log::warn("{}: Skipping '{}': {}", logName(), displayPath(index), paths.error().toString());
            failedEntries_.emplace_back(failureSource(index), paths.error());
            failedEntryIndices_.push_back(static_cast<std::uint32_t>(index));
            completedBytes_ += entry.isRegularFile() ? entry.size : 0u;
            ++completedEntries_;
            continue;
        }
        if (entry.isDirectory())
        {
            // Created inline while the cursor passes it, so files listed after it only start once
            // it exists. Scan order lists parents before children.
            const auto result = createDirectoryInStrand(sftp, index, *paths);
            if (!result.has_value())
            {
                // Without a tree, nothing else depends on a directory entry.
                if (!prescanned() && !isSkippableDirectoryError(result.error()))
                    return std::unexpected(result.error());
                Log::warn("{}: Skipping directory '{}': {}", logName(), displayPath(index), result.error().toString());
                failedEntries_.emplace_back(failureSource(index), result.error());
                failedEntryIndices_.push_back(static_cast<std::uint32_t>(index));
            }
            else
            {
                finishedEntries_[index] = true;
            }
            ++completedEntries_;
            ++directoriesCreated;
            continue;
        }
        if (entry.isRegularFile() || entry.isSymlink())
        {
            auto child = makeChild(
                index,
                *paths,
                [this, index](std::uint64_t current, std::uint64_t max)
                {
                    if (auto* slot = slotFor(index); slot)
                    {
                        slot->bytes = current;
                        slot->totalBytes = max;
                    }
                }
            );
            slots_.push_back(Slot{.entryIndex = index, .child = std::move(child), .totalBytes = entry.size});
            continue;
        }
        Log::warn("{}: Skipping unsupported file type for entry: {}.", logName(), displayPath(index));
        finishedEntries_[index] = true;
        ++completedEntries_;
    }
    return {};
}

std::expected<void, BulkTransferOperation::Error> BulkTransferOperation::stepSlots()
{
    for (auto& slot : slots_)
    {
        auto result = slot.child->workInStrand();
        if (result.has_value())
        {
            if (result.value() == WorkStatus::Complete)
                finishSlot(slot, true);
            continue;
        }

        const auto error = result.error();
        if (isBufferShortage(error) && slot.bytes == 0)
        {
            // The transfer buffer pool is exhausted, most likely by our own siblings. Put the
            // entry back and try again once a slot frees up instead of failing the bulk.
            if (!bufferShortageLogged_)
            {
                Log::warn("{}: Transfer buffers exhausted, deferring entries until a slot frees up.", logName());
                bufferShortageLogged_ = true;
            }
            deferred_.push_back(slot.entryIndex);
            retryDeferred_ = false;
            slot.child.reset();
            slot.finished = true;
            continue;
        }

        Log::error("{}: Transfer failed for: {}: {}", logName(), displayPath(slot.entryIndex), error.toString());
        if (isSkippableError(error))
        {
            // Not critical for the bulk as a whole: record the entry for the user and move on.
            failedEntries_.emplace_back(failureSource(slot.entryIndex), error);
            failedEntryIndices_.push_back(static_cast<std::uint32_t>(slot.entryIndex));
            finishSlot(slot, false);
            if (common_.failFast)
            {
                Log::error("{}: failFast is enabled, failing the whole operation due to the error.", logName());
                return std::unexpected(error);
            }
            continue;
        }
        return std::unexpected(error);
    }
    return {};
}

void BulkTransferOperation::finishSlot(Slot& slot, bool transferred)
{
    if (transferred)
        finishedEntries_[slot.entryIndex] = true;
    // Advance by the entry's reported size, not the child's measured size: totalBytes_ is
    // summed from the same reported sizes, so the two converge exactly.
    completedBytes_ += entries_[slot.entryIndex].size;
    ++completedEntries_;
    slot.child.reset();
    slot.finished = true;
    retryDeferred_ = true;
}

BulkTransferOperation::Slot* BulkTransferOperation::slotFor(std::size_t entryIndex) noexcept
{
    const auto it = std::find_if(
        slots_.begin(),
        slots_.end(),
        [entryIndex](Slot const& slot)
        {
            return slot.entryIndex == entryIndex;
        }
    );
    return it == slots_.end() ? nullptr : &*it;
}

std::uint64_t BulkTransferOperation::fileCount() const noexcept
{
    // Tree mode treats entries_[0] as the synthetic root (not counted toward the user-visible
    // total); prescanned mode has no root, so every entry counts.
    if (entries_.empty())
        return 0;
    return entries_.size() - (prescanned() ? 0 : 1);
}

std::uint64_t BulkTransferOperation::bytesNow() const noexcept
{
    // Clamp to the listed size: completion adds that size, so a file that grew since the
    // listing must not make the total step backwards.
    std::uint64_t inFlight = 0;
    for (auto const& slot : slots_)
        inFlight += std::min(slot.bytes, entries_[slot.entryIndex].size);
    return completedBytes_ + inFlight;
}

void BulkTransferOperation::emitProgress(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && (now - lastEmit_) < common_.progressEmitInterval)
        return;
    lastEmit_ = now;

    SharedData::BulkProgress update{
        .operationId = {},
        .fileCurrentIndex = completedEntries_,
        .fileCount = fileCount(),
        .bytesCurrent = bytesNow(),
        .bytesTotal = totalBytes_,
        .bytesPerSecond = 0,
        .inFlight = {},
        .failedEntryIndices = failedEntryIndices_,
    };
    update.bytesPerSecond = updateBulkBytesPerSecond(update.bytesCurrent);
    update.inFlight.reserve(slots_.size() + deferred_.size());
    for (auto const& slot : slots_)
    {
        if (slot.finished)
            continue;
        update.inFlight.push_back(
            SharedData::BulkFileProgress{
                .file = displayPath(slot.entryIndex),
                .entryIndex = slot.entryIndex,
                .bytes = slot.bytes,
                .totalBytes = slot.totalBytes,
            }
        );
    }
    // Deferred entries stay listed so consumers do not take their absence for completion.
    for (const auto entryIndex : deferred_)
    {
        update.inFlight.push_back(
            SharedData::BulkFileProgress{
                .file = displayPath(entryIndex),
                .entryIndex = entryIndex,
                .bytes = 0,
                .totalBytes = entries_[entryIndex].size,
            }
        );
    }
    common_.overallProgressCallback(std::move(update));
}

bool BulkTransferOperation::isBufferShortage(Error const& error) noexcept
{
    return error.sftpError.has_value() &&
        error.sftpError->wrapperError == SecureShell::WrapperErrors::BufferUnavailable;
}

std::int64_t BulkTransferOperation::updateBulkBytesPerSecond(std::uint64_t bytesNow)
{
    // Sample cadence and smoothing factor chosen so the displayed rate updates slowly enough to
    // be readable. EWMA dampens the jump between samples.
    constexpr auto sampleInterval = 1000ms;
    constexpr double smoothingAlpha = 0.3;

    const auto now = std::chrono::steady_clock::now();
    if (lastBpsSampleTime_.time_since_epoch().count() == 0)
    {
        lastBpsSampleTime_ = now;
        lastBpsSampleBytes_ = bytesNow;
        return bulkBytesPerSecond_;
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBpsSampleTime_);
    if (duration < sampleInterval)
        return bulkBytesPerSecond_;
    const std::uint64_t bytesDelta = bytesNow >= lastBpsSampleBytes_ ? (bytesNow - lastBpsSampleBytes_) : 0;
    const double rawBps = static_cast<double>(bytesDelta * 1000) / static_cast<double>(duration.count());
    // First real sample seeds the average directly to avoid a slow ramp-up from zero.
    const double smoothed = bulkBytesPerSecond_ == 0
        ? rawBps
        : smoothingAlpha * rawBps + (1.0 - smoothingAlpha) * static_cast<double>(bulkBytesPerSecond_);
    bulkBytesPerSecond_ = static_cast<std::int64_t>(smoothed);
    lastBpsSampleTime_ = now;
    lastBpsSampleBytes_ = bytesNow;
    return bulkBytesPerSecond_;
}

int BulkTransferOperation::parallelWorkDoable(int parallel) const noexcept
{
    const auto pending = static_cast<std::int64_t>(entries_.size() - std::min(cursor_, entries_.size())) +
        static_cast<std::int64_t>(deferred_.size()) + static_cast<std::int64_t>(slots_.size());
    const auto wanted = std::min<std::int64_t>({parallel, common_.concurrency, pending});
    return static_cast<int>(std::clamp<std::int64_t>(wanted, 1, std::max(1, parallel)));
}

void BulkTransferOperation::setParallelBudget(int slots)
{
    budget_ = std::clamp(slots, 1, std::max(1, common_.concurrency));
}

void BulkTransferOperation::setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes)
{
    entries_ = std::move(entries);
    finishedEntries_.assign(entries_.size(), false);
    totalBytes_ = totalBytes;
    prescannedPaths_.clear();
    entrySource_ = EntrySource::Tree;
}

void BulkTransferOperation::setPrescannedEntries(
    std::vector<SharedData::DirectoryEntry> entries,
    std::vector<EntryPaths> paths,
    std::uint64_t totalBytes
)
{
    entries_ = std::move(entries);
    finishedEntries_.assign(entries_.size(), false);
    prescannedPaths_ = std::move(paths);
    totalBytes_ = totalBytes;
    entrySource_ = EntrySource::Flat;
}

void BulkTransferOperation::addRejectedEntries(std::vector<std::string> const& paths)
{
    for (auto const& path : paths)
    {
        Log::warn("{}: Skipping '{}', its name is not a valid path segment.", logName(), path);
        failedEntries_.emplace_back(
            path, Error{.type = ErrorType::InvalidName, .extraInfo = "The name is not a valid path segment"}
        );
    }
}

void BulkTransferOperation::addSkippedLinks(std::vector<std::string> const& paths)
{
    for (auto const& path : paths)
    {
        Log::warn("{}: Skipping the link '{}', links are not transferred from here.", logName(), path);
        failedEntries_.emplace_back(
            path,
            Error{
                .type = ErrorType::OperationNotPossibleOnFileType,
                .extraInfo = "Links and junctions are not transferred"
            }
        );
    }
}

void BulkTransferOperation::addUnreadableDirectories(std::vector<std::string> const& paths)
{
    for (auto const& path : paths)
    {
        Log::warn("{}: Skipping the content of '{}', it cannot be listed.", logName(), path);
        failedEntries_.emplace_back(
            path, Error{.type = ErrorType::DirectoryUnreadable, .extraInfo = "The directory cannot be listed"}
        );
    }
}

std::vector<std::pair<std::string, BulkTransferOperation::Error>> BulkTransferOperation::getFailed() const
{
    return failedEntries_;
}

std::vector<std::size_t> BulkTransferOperation::unfinishedEntryIndices() const
{
    std::vector<std::size_t> unfinished;
    for (std::size_t index = 0; index < finishedEntries_.size(); ++index)
    {
        if (!finishedEntries_[index])
            unfinished.push_back(index);
    }
    return unfinished;
}

std::expected<BulkTransferOperation::EntryPaths, BulkTransferOperation::Error>
BulkTransferOperation::entryPaths(std::size_t entryIndex) const
{
    if (prescanned())
        return prescannedPaths_[entryIndex];
    if (common_.remotePath.empty() || common_.localPath.empty())
        return std::unexpected(Error{.type = ErrorType::ImplementationError, .extraInfo = "A tree needs both roots"});

    const auto invalidName = [](Paths::PathError const& error)
    {
        return Error{.type = ErrorType::InvalidName, .extraInfo = error.toString()};
    };

namespace Detail
{
    std::expected<BulkTransferOperation::EntryPaths, Operation::Error> treeEntryPaths(
        std::vector<SharedData::DirectoryEntry> const& entries,
        std::size_t entryIndex,
        Paths::RemotePath const& remoteRoot,
        Paths::LocalPath const& localRoot
    )
    {
        using ErrorType = Operation::ErrorType;
        if (remoteRoot.empty() || localRoot.empty())
            return std::unexpected(
                Operation::Error{.type = ErrorType::ImplementationError, .extraInfo = "A tree needs both roots"}
            );
        if (entryIndex >= entries.size())
            return std::unexpected(
                Operation::Error{.type = ErrorType::ImplementationError, .extraInfo = "Entry out of range"}
            );

        const auto refused = [](Paths::PathError const& error)
        {
            if (error.reason == Paths::PathRejection::UnknownParent)
                return Operation::Error{.type = ErrorType::ImplementationError, .extraInfo = "Parent out of range"};
            return Operation::Error{.type = ErrorType::InvalidName, .extraInfo = error.toString()};
        };
        return Utility::walkedRelativePath(entries, entries[entryIndex])
            .and_then(
                [&](Paths::RelativePath const& relative)
                {
                    return localRoot.join(relative).transform(
                        [&](Paths::LocalPath local)
                        {
                            return BulkTransferOperation::EntryPaths{
                                .remote = remoteRoot / relative, .local = std::move(local)
                            };
                        }
                    );
                }
            )
            .transform_error(refused);
    }

    return common_.localPath.join(relative)
        .transform(
            [this, &relative](Paths::LocalPath local)
            {
                return EntryPaths{.remote = common_.remotePath / relative, .local = std::move(local)};
            }
        )
        .transform_error(invalidName);
}

std::string BulkTransferOperation::displayPath(std::size_t entryIndex) const
{
    if (auto paths = entryPaths(entryIndex); paths)
        return paths->remote.value();
    return describeEntry(entryIndex);
}

std::string BulkTransferOperation::failureSource(std::size_t entryIndex) const
{
    if (auto paths = entryPaths(entryIndex); paths)
        return sourceIsRemote() ? paths->remote.value() : paths->local.toUtf8();
    return describeEntry(entryIndex);
}

std::string BulkTransferOperation::describeEntry(std::size_t entryIndex) const
{
    // The names as listed, one of them is no path segment or the entry would have paths.
    std::vector<std::string> names;
    for (auto const* current = &entries_[entryIndex]; current->parent && *current->parent < entries_.size();
         current = &entries_[*current->parent])
    {
        names.push_back(current->name);
    }
    auto root = sourceIsRemote() ? common_.remotePath.value() : common_.localPath.toUtf8();
    for (auto name = names.rbegin(); name != names.rend(); ++name)
    {
        if (!root.ends_with('/'))
            root.push_back('/');
        root += *name;
    }
    return root;
}

void BulkTransferOperation::cancelChildrenInStrand(bool adoptCancelState)
{
    for (auto& slot : slots_)
    {
        if (slot.child)
            std::ignore = slot.child->cancel(adoptCancelState);
    }
    slots_.clear();
    deferred_.clear();
}

std::expected<void, BulkTransferOperation::Error> BulkTransferOperation::cancel(bool adoptCancelState)
{
    if (scanPhase_)
        scanPhase_->cancel(adoptCancelState);

    // Each child's cancel and destructor would block on the processing thread separately;
    // hop once and do all of them there.
    const auto sftp = openSession(sftp_);
    if (!slots_.empty() && sftp && !sftp->strand()->withinProcessingThread())
    {
        // Only wait, never get(): a finalized strand hands back an exceptional future and this
        // runs from the destructor too.
        auto fut = sftp->performPromise(
            [this, adoptCancelState]() -> bool
            {
                cancelChildrenInStrand(adoptCancelState);
                return true;
            }
        );
        if (fut.wait_for(common_.futureTimeout) != std::future_status::ready)
            Log::error("{}: cancel umbrella timed out, children stay with the processing thread.", logName());
    }
    else
    {
        cancelChildrenInStrand(adoptCancelState);
    }
    if (adoptCancelState)
        enterState(OperationState::Canceled);
    return {};
}

void BulkTransferOperation::pause(bool doPause)
{
    for (auto& slot : slots_)
    {
        if (slot.child)
            slot.child->pause(doPause);
    }
}

SecureShell::ProcessingStrand* BulkTransferOperation::strand() const
{
    return strandOf(sftp_);
}
