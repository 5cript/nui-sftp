#include <frontend/session_components/file_tracking/tracking_store.hpp>
#include <utility/enum_string_convert.hpp>

#include <array>
#include <cstddef>
#include <utility>

namespace FileTracking
{
    struct TrackingStore::Implementation
    {
        explicit Implementation(FileTrackingTexts texts)
            : texts{std::move(texts)}
        {}

        FileTrackingTexts texts;
        TrackingModel model{};
        std::array<Nui::Observed<std::vector<std::shared_ptr<WatchItem>>>, outlinerGroupOrder.size()> members{};
        Nui::Observed<std::vector<std::shared_ptr<OrphanItem>>> orphans{};
        std::array<std::shared_ptr<Nui::Observed<std::uint64_t>>, outlinerGroupOrder.size()> counts{
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
        };
        Nui::Observed<Selection> selection{};
        DetailTab detailTab{DetailTab::Conflicts};
        std::shared_ptr<Nui::Observed<std::string>> detailTabKey{
            std::make_shared<Nui::Observed<std::string>>(Utility::enumToString(DetailTab::Conflicts))
        };
        Nui::Observed<std::string> filter{};
        std::array<bool, outlinerGroupOrder.size()> expanded{true, true, true, true, true};
        std::function<void(AttentionCounts const& counts)> onAttentionChanged{};
    };

    namespace
    {
        std::size_t indexOf(OutlinerGroup group)
        {
            return static_cast<std::size_t>(group);
        }
    }

    TrackingStore::TrackingStore(FileTrackingTexts texts)
        : impl_{std::make_unique<Implementation>(std::move(texts))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_MOVE(TrackingStore);

    void TrackingStore::showEntry([[maybe_unused]] SharedData::TrackedEntry const& entry)
    {}

    void TrackingStore::removeEntry([[maybe_unused]] Ids::WatchId const& watchId)
    {}

    void TrackingStore::showConflict([[maybe_unused]] SharedData::WatchConflict const& conflict)
    {}

    void TrackingStore::removeConflict([[maybe_unused]] Ids::ConflictId const& conflictId)
    {}

    void TrackingStore::showFailure([[maybe_unused]] SharedData::UploadFailure const& failure)
    {}

    void TrackingStore::removeFailure([[maybe_unused]] Ids::FailureId const& failureId)
    {}

    void TrackingStore::showWatchProgress([[maybe_unused]] SharedData::WatchProgress const& progress)
    {}

    void TrackingStore::showOrphanedCopies([[maybe_unused]] SharedData::OrphanedCopies const& copies)
    {}

    void TrackingStore::clear()
    {}

    TrackingModel const& TrackingStore::model() const
    {
        return impl_->model;
    }

    FileTrackingTexts const& TrackingStore::texts() const
    {
        return impl_->texts;
    }

    Nui::Observed<std::vector<std::shared_ptr<WatchItem>>>& TrackingStore::membersOf(OutlinerGroup group)
    {
        return impl_->members[indexOf(group)];
    }

    Nui::Observed<std::vector<std::shared_ptr<OrphanItem>>>& TrackingStore::orphanMembers()
    {
        return impl_->orphans;
    }

    std::shared_ptr<Nui::Observed<std::uint64_t>> TrackingStore::countOf(OutlinerGroup group)
    {
        return impl_->counts[indexOf(group)];
    }

    std::shared_ptr<WatchItem> TrackingStore::watchItem([[maybe_unused]] Ids::WatchId const& watchId) const
    {
        return nullptr;
    }

    std::shared_ptr<OrphanItem> TrackingStore::orphanItem([[maybe_unused]] Ids::InstanceId const& instanceId) const
    {
        return nullptr;
    }

    Nui::Observed<Selection>& TrackingStore::selection()
    {
        return impl_->selection;
    }

    void TrackingStore::select([[maybe_unused]] Selection selection)
    {}

    void TrackingStore::moveSelection([[maybe_unused]] SelectionMove move)
    {}

    DetailTab TrackingStore::detailTab() const
    {
        return impl_->detailTab;
    }

    void TrackingStore::setDetailTab([[maybe_unused]] DetailTab tab)
    {}

    std::shared_ptr<Nui::Observed<std::string>> TrackingStore::detailTabKey()
    {
        return impl_->detailTabKey;
    }

    Nui::Observed<std::string>& TrackingStore::filter()
    {
        return impl_->filter;
    }

    void TrackingStore::setFilter([[maybe_unused]] std::string filter)
    {}

    bool TrackingStore::groupExpanded(OutlinerGroup group) const
    {
        return impl_->expanded[indexOf(group)];
    }

    void TrackingStore::setGroupExpanded([[maybe_unused]] OutlinerGroup group, [[maybe_unused]] bool expanded)
    {}

    bool TrackingStore::focusNewestConflict()
    {
        return false;
    }

    void TrackingStore::onAttentionChanged(std::function<void(AttentionCounts const& counts)> onChanged)
    {
        impl_->onAttentionChanged = std::move(onChanged);
    }
}
