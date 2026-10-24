#include <frontend/session_components/file_tracking/tracking_model.hpp>

namespace FileTracking
{
    bool matchesFilter([[maybe_unused]] std::string_view name, [[maybe_unused]] std::string_view filter)
    {
        return true;
    }

    void TrackingModel::showEntry([[maybe_unused]] SharedData::TrackedEntry const& entry)
    {}

    void TrackingModel::removeEntry([[maybe_unused]] Ids::WatchId const& watchId)
    {}

    void TrackingModel::showConflict([[maybe_unused]] SharedData::WatchConflict const& conflict)
    {}

    void TrackingModel::removeConflict([[maybe_unused]] Ids::ConflictId const& conflictId)
    {}

    void TrackingModel::showFailure([[maybe_unused]] SharedData::UploadFailure const& failure)
    {}

    void TrackingModel::removeFailure([[maybe_unused]] Ids::FailureId const& failureId)
    {}

    void TrackingModel::showWatchProgress([[maybe_unused]] SharedData::WatchProgress const& progress)
    {}

    void TrackingModel::showOrphanedCopies([[maybe_unused]] SharedData::OrphanedCopies const& copies)
    {}

    void TrackingModel::clear()
    {}

    SharedData::TrackedEntry const* TrackingModel::entryOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return nullptr;
    }

    SharedData::WatchProgress const* TrackingModel::progressOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return nullptr;
    }

    SharedData::OrphanedCopy const* TrackingModel::orphanOf([[maybe_unused]] Ids::InstanceId const& instanceId) const
    {
        return nullptr;
    }

    std::optional<Ids::WatchId> TrackingModel::watchOfConflict([[maybe_unused]] Ids::ConflictId const& conflictId) const
    {
        return std::nullopt;
    }

    std::optional<Ids::WatchId> TrackingModel::watchOfFailure([[maybe_unused]] Ids::FailureId const& failureId) const
    {
        return std::nullopt;
    }

    std::vector<SharedData::WatchConflict>
    TrackingModel::conflictsOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return {};
    }

    std::vector<SharedData::UploadFailure> TrackingModel::failuresOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return {};
    }

    std::uint64_t TrackingModel::conflictCountOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return 0u;
    }

    std::uint64_t TrackingModel::failureCountOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return 0u;
    }

    std::uint64_t TrackingModel::conflictCount() const
    {
        return 0u;
    }

    std::uint64_t TrackingModel::failureCount() const
    {
        return 0u;
    }

    WatchStatus TrackingModel::statusOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return WatchStatus::Watching;
    }

    OutlinerGroup TrackingModel::groupOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return OutlinerGroup::Watching;
    }

    std::vector<Selection>
    TrackingModel::members([[maybe_unused]] OutlinerGroup group, [[maybe_unused]] std::string_view filter) const
    {
        return {};
    }

    std::string TrackingModel::sideTextOf(
        [[maybe_unused]] Ids::WatchId const& watchId,
        [[maybe_unused]] FileTrackingTexts const& texts
    ) const
    {
        return {};
    }

    std::string TrackingModel::statusTextOf(
        [[maybe_unused]] Ids::WatchId const& watchId,
        [[maybe_unused]] FileTrackingTexts const& texts
    ) const
    {
        return {};
    }

    double TrackingModel::ringFractionOf([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return 0.0;
    }

    std::optional<SharedData::WatchConflict> TrackingModel::newestConflict() const
    {
        return std::nullopt;
    }
}
