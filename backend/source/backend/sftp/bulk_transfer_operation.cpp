#include <backend/sftp/bulk_transfer_operation.hpp>
#include <log/log.hpp>

#include <algorithm>

using namespace std::chrono_literals;

BulkTransferOperation::BulkTransferOperation(SecureShell::SftpSession& sftp, CommonOptions common)
    : Operation{}
    , sftp_{&sftp}
    , common_{std::move(common)}
    , budget_{std::max(1, common_.concurrency)}
{}

BulkTransferOperation::~BulkTransferOperation()
{
    std::ignore = cancel(false);
}

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error> BulkTransferOperation::work()
{
    auto fut = sftp_->performPromise(
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

    switch (state())
    {
        case (NotStarted):
            return start();
        case (Preparing):
            [[fallthrough]];
        case (Prepared):
            [[fallthrough]];
        case (Running):
            return step();
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

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error> BulkTransferOperation::start()
{
    if (entries_.empty())
    {
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
        if (!root.isDirectory())
        {
            Log::error("{}: First entry is not a directory: {}.", logName(), root.path.string());
            return enterErrorState<WorkStatus>(
                Error{.type = ErrorType::ImplementationError, .extraInfo = "First entry must be a directory."}
            );
        }
        const auto result = prepareRootInStrand(root);
        if (!result.has_value())
            return enterErrorState<WorkStatus>(result.error());
        cursor_ = 1;
    }

    enterState(OperationState::Running);
    emitProgress(true);
    return WorkStatus::MoreWork;
}

std::expected<BulkTransferOperation::WorkStatus, BulkTransferOperation::Error> BulkTransferOperation::step()
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
    if (const auto filled = fillSlots(); !filled.has_value())
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
        emitProgress(true);
        Log::info("{}: Bulk transfer completed.", logName());
        enterState(OperationState::Completed);
        return WorkStatus::Complete;
    }

    emitProgress(false);
    return WorkStatus::MoreWork;
}

std::expected<void, BulkTransferOperation::Error> BulkTransferOperation::fillSlots()
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
        if (entry.isDirectory())
        {
            // Created inline while the cursor passes it, so files listed after it only start once
            // it exists. Scan order lists parents before children.
            const auto result = createDirectoryInStrand(index);
            if (!result.has_value())
                return std::unexpected(result.error());
            ++completedEntries_;
            ++directoriesCreated;
            continue;
        }
        if (entry.isRegularFile() || entry.isSymlink())
        {
            auto child = makeChild(
                index,
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
        Log::warn("{}: Skipping unsupported file type for entry: {}.", logName(), displayPath(index).string());
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
                finishSlot(slot);
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

        Log::error(
            "{}: Transfer failed for: {}: {}", logName(), displayPath(slot.entryIndex).string(), error.toString()
        );
        if (isSkippableError(error))
        {
            // Not critical for the bulk as a whole: record the entry for the user and move on.
            failedEntries_.emplace_back(SharedData::fullPath(entries_, entries_[slot.entryIndex]), error);
            failedEntryIndices_.push_back(static_cast<std::uint32_t>(slot.entryIndex));
            finishSlot(slot);
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

void BulkTransferOperation::finishSlot(Slot& slot)
{
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
    std::uint64_t inFlight = 0;
    for (auto const& slot : slots_)
        inFlight += slot.bytes;
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
                .file = displayPath(slot.entryIndex).string(),
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
                .file = displayPath(entryIndex).string(),
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
    totalBytes_ = totalBytes;
    prescannedDestinations_.clear();
}

void BulkTransferOperation::setPrescannedEntries(
    std::vector<SharedData::DirectoryEntry> entries,
    std::vector<std::filesystem::path> destinations,
    std::uint64_t totalBytes
)
{
    entries_ = std::move(entries);
    prescannedDestinations_ = std::move(destinations);
    totalBytes_ = totalBytes;
}

std::vector<std::pair<std::filesystem::path, BulkTransferOperation::Error>> BulkTransferOperation::getFailed() const
{
    return failedEntries_;
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
    // Each child's cancel and destructor would block on the processing thread separately;
    // hop once and do all of them there.
    auto* processingStrand = sftp_->strand();
    if (!slots_.empty() && processingStrand && !processingStrand->withinProcessingThread())
    {
        // Only wait, never get(): a finalized strand hands back an exceptional future and this
        // runs from the destructor too.
        auto fut = sftp_->performPromise(
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
    return sftp_->strand();
}
