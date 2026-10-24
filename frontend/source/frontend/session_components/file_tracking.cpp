#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/file_tracking/dock_tab_attention.hpp>
#include <frontend/session_components/file_tracking/orphan_detail.hpp>
#include <frontend/session_components/file_tracking/outliner.hpp>
#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <frontend/session_components/file_tracking/tracker_client.hpp>
#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>
#include <frontend/session_components/file_tracking/translated_texts.hpp>
#include <frontend/session_components/file_tracking/watch_detail.hpp>
#include <frontend/file_explorer/open_local_file.hpp>
#include <frontend/notifications.hpp>

#include <shared_data/file_tracking/ended_watches.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>

#include <script-nui-components/style_variant.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

struct FileTrackingPanel::Implementation
{
    explicit Implementation(Params params)
        : confirmDialog{params.confirmDialog}
        , store{FileTracking::translatedTexts()}
        , client{
              store,
              [](SharedData::EndedWatches const& ended)
              {
                  notifyEnded(ended);
              }
          }
        , attention{std::move(params.sessionLayoutId)}
    {
        store.onAttentionChanged(
            [this](FileTracking::AttentionCounts const& counts)
            {
                attention.show(counts);
            }
        );
        actions = makeActions();
    }

    ConfirmDialog* confirmDialog;
    FileTracking::TrackingStore store;
    FileTracking::TrackerClient client;
    FileTracking::DockTabAttention attention;
    FileTracking::TrackingActions actions{};
    std::function<void(Paths::RemotePath const& path)> revealOnServer{};
    Utility::LifetimeToken lifetime{};

    /**
     * @brief Tells the user why watches ended that they did not stop.
     */
    static void notifyEnded(SharedData::EndedWatches const& ended)
    {
        if (ended.entries.empty())
            return;
        const auto text = [&ended]()
        {
            switch (ended.reason)
            {
                case SharedData::WatchEndReason::CopyGone:
                    return language->get("fileTracking", "watchEndedCopyGone");
                case SharedData::WatchEndReason::Overlap:
                    return language->get("fileTracking", "watchEndedOverlap");
                case SharedData::WatchEndReason::ConnectionLost:
                    break;
            }
            return language->get("fileTracking", "watchesEndedConnectionLost");
        }();
        const auto names = ended.entries |
            std::views::transform(
                [](SharedData::TrackedEntry const& entry)
                {
                    return fmt::format("'{}'", entry.name);
                }
            );
        Notifications::warning(fmt::format(fmt::runtime(text), fmt::join(names, ", ")));
    }

    /**
     * @brief Opens the copy @p localPath of @p instanceId with its application, discarding it when the user declines
     *        to open a risky file, like Open and Watch in the file explorer.
     */
    void openCopy(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
    {
        FileExplorer::openLocalFile(
            lifetime,
            *confirmDialog,
            localPath,
            false,
            lifetime.guarded(
                [this, instanceId, localPath]()
                {
                    client.discard(instanceId, localPath);
                }
            )
        );
    }

    /**
     * @brief Opens the server file of the stopped copy @p watchId again and watches it, then removes the stopped one.
     */
    void openAgain(Ids::WatchId const& watchId)
    {
        auto const* const entry = store.model().entryOf(watchId);
        if (entry == nullptr)
            return;
        client.open(
            entry->remotePath,
            0u,
            true,
            lifetime.guarded(
                [this, watchId](Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
                {
                    client.removeStopped(watchId);
                    openCopy(instanceId, localPath);
                }
            ),
            lifetime.guarded(
                [](std::string const& reason)
                {
                    Notifications::error(reason);
                }
            )
        );
    }

    void deleteOrphanedCopy(Ids::InstanceId const& instanceId)
    {
        auto const* const copy = store.model().orphanOf(instanceId);
        if (copy == nullptr)
            return;
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("fileTracking", "confirmDeleteOrphanHeader"),
            .text = fmt::format(fmt::runtime(language->get("fileTracking", "confirmDeleteOrphanText")), copy->name),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::No,
            .onClose = lifetime.guarded(
                [this, instanceId, name = copy->name](std::optional<ConfirmDialog::Button> button)
                {
                    if (button != ConfirmDialog::Button::Yes)
                        return;
                    client.removeOrphanedCopy(
                        instanceId,
                        lifetime.guarded(
                            [name](std::string const& error)
                            {
                                Notifications::error(fmt::format(
                                    fmt::runtime(language->get("fileTracking", "deleteOrphanFailed")),
                                    name,
                                    language->translate(error)
                                ));
                            }
                        )
                    );
                }
            ),
        });
    }

    /**
     * @brief Asks before deleting every listed orphaned copy; a single one is asked for by its name.
     */
    void deleteAllOrphanedCopies()
    {
        const auto orphans = store.model().members(FileTracking::OutlinerGroup::Orphaned, "");
        if (orphans.empty())
            return;
        if (orphans.size() == 1u)
        {
            if (auto const* const instanceId = std::get_if<Ids::InstanceId>(&orphans.front()); instanceId != nullptr)
                deleteOrphanedCopy(*instanceId);
            return;
        }
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("fileTracking", "confirmDeleteAllOrphansHeader"),
            .text = fmt::format(
                fmt::runtime(language->get("fileTracking", "confirmDeleteAllOrphansText")), orphans.size()
            ),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::No,
            .onClose = lifetime.guarded(
                [this](std::optional<ConfirmDialog::Button> button)
                {
                    if (button == ConfirmDialog::Button::Yes)
                        client.removeAllOrphanedCopies();
                }
            ),
        });
    }

    FileTracking::TrackingActions makeActions()
    {
        return {
            .answerConflict =
                [this](Ids::ConflictId const& conflictId, SharedData::ConflictResolution resolution)
            {
                client.answerConflict(conflictId, resolution);
            },
            .answerAllConflicts =
                [this](Ids::WatchId const& watchId, SharedData::ConflictResolution resolution)
            {
                client.answerAllConflicts(watchId, resolution);
            },
            .retryFailure =
                [this](Ids::FailureId const& failureId)
            {
                client.retryFailure(failureId);
            },
            .dismissFailure =
                [this](Ids::FailureId const& failureId)
            {
                client.dismissFailure(failureId);
            },
            .retryAllFailures =
                [this](Ids::WatchId const& watchId)
            {
                client.retryAllFailures(watchId);
            },
            .dismissAllFailures =
                [this](Ids::WatchId const& watchId)
            {
                client.dismissAllFailures(watchId);
            },
            .setOption =
                [this](Ids::WatchId const& watchId, SharedData::WatchOption option, bool enabled)
            {
                client.setOption(watchId, option, enabled);
            },
            .stop =
                [this](Ids::WatchId const& watchId)
            {
                client.stop(watchId);
            },
            .removeStopped =
                [this](Ids::WatchId const& watchId)
            {
                client.removeStopped(watchId);
            },
            .removeAllStopped =
                [this]()
            {
                client.removeAllStopped();
            },
            .openAgain =
                [this](Ids::WatchId const& watchId)
            {
                openAgain(watchId);
            },
            .showWatch =
                [this](Ids::WatchId const& otherWatchId)
            {
                store.select(otherWatchId);
            },
            .deleteOrphanedCopy =
                [this](Ids::InstanceId const& instanceId)
            {
                deleteOrphanedCopy(instanceId);
            },
            .deleteAllOrphanedCopies =
                [this]()
            {
                deleteAllOrphanedCopies();
            },
            .openLocally =
                [this](Paths::LocalPath const& path)
            {
                FileExplorer::openLocalFile(lifetime, *confirmDialog, path, false);
            },
            .revealOnServer =
                [this](Paths::RemotePath const& path)
            {
                if (revealOnServer)
                    revealOnServer(path);
            },
            .select =
                [this](FileTracking::Selection const& selection)
            {
                store.select(selection);
            },
            .moveSelection =
                [this](FileTracking::SelectionMove move)
            {
                store.moveSelection(move);
            },
            .chooseTab =
                [this](FileTracking::DetailTab tab)
            {
                store.setDetailTab(tab);
            },
            .filter =
                [this](std::string const& filter)
            {
                store.setFilter(filter);
            },
            .setGroupExpanded =
                [this](FileTracking::OutlinerGroup group, bool expanded)
            {
                store.setGroupExpanded(group, expanded);
            },
        };
    }

    /**
     * @return Whether the tracker lists nothing at all, whatever the filter hides.
     */
    bool nothingTracked() const
    {
        return std::ranges::all_of(
            FileTracking::outlinerGroupOrder,
            [this](FileTracking::OutlinerGroup group)
            {
                return store.model().members(group, "").empty();
            }
        );
    }

    Nui::ElementRenderer renderPlaceholder()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using FileTracking::OutlinerGroup;

        // clang-format off
        return div{
            class_ = "file-tracking-panel-placeholder",
            "data-ft-detail"_attr = "none",
        }(
            Nui::observe(
                *store.countOf(OutlinerGroup::NeedsYou),
                *store.countOf(OutlinerGroup::Syncing),
                *store.countOf(OutlinerGroup::Watching),
                *store.countOf(OutlinerGroup::Stopped),
                *store.countOf(OutlinerGroup::Orphaned),
                store.filter()
            ).generate([this]() -> Nui::ElementRenderer {
                return Nui::Elements::text{
                    language->get("fileTracking", nothingTracked() ? "nothingTracked" : "nothingSelected")
                }();
            })
        );
        // clang-format on
    }

    /**
     * @brief The detail of what the outliner selects, the placeholder while that is nothing or not known.
     */
    Nui::ElementRenderer renderDetail()
    {
        auto const& selection = store.selection().value();
        if (auto const* const watchId = std::get_if<Ids::WatchId>(&selection); watchId != nullptr)
        {
            if (const auto item = store.watchItem(*watchId); item)
                return FileTracking::watchDetail(store, actions, item);
        }
        if (auto const* const instanceId = std::get_if<Ids::InstanceId>(&selection); instanceId != nullptr)
        {
            if (const auto item = store.orphanItem(*instanceId); item)
                return FileTracking::orphanDetail(store, actions, item);
        }
        return renderPlaceholder();
    }
};

FileTrackingPanel::FileTrackingPanel(Params params)
    : impl_(std::make_unique<Implementation>(std::move(params)))
{}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FileTrackingPanel);

void FileTrackingPanel::activate(Ids::SessionId sessionId)
{
    impl_->client.activate(std::move(sessionId));
    impl_->attention.reset();
}

void FileTrackingPanel::deactivate()
{
    impl_->client.deactivate();
    impl_->attention.reset();
}

void FileTrackingPanel::open(
    Paths::RemotePath const& remotePath,
    std::uint64_t sizeBytes,
    bool watch,
    std::function<void(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)> onOpened,
    std::function<void(std::string const& reason)> onRefused
)
{
    impl_->client.open(remotePath, sizeBytes, watch, std::move(onOpened), std::move(onRefused));
}

void FileTrackingPanel::checkUploadAndWatch(
    Paths::LocalPath const& item,
    Paths::RemotePath const& directory,
    std::function<void(Paths::LocalPath const& canonicalItem)> onWatchable,
    std::function<void(std::string const& reason)> onRefused
)
{
    impl_->client.checkUploadAndWatch(item, directory, std::move(onWatchable), std::move(onRefused));
}

void FileTrackingPanel::uploadAndWatch(
    Paths::LocalPath const& item,
    Paths::RemotePath const& remotePath,
    std::function<void(std::string const& reason)> onRefused
)
{
    impl_->client.uploadAndWatch(item, remotePath, std::move(onRefused));
}

void FileTrackingPanel::discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
{
    impl_->client.discard(instanceId, localPath);
}

void FileTrackingPanel::setRevealOnServer(std::function<void(Paths::RemotePath const& path)> revealOnServer)
{
    impl_->revealOnServer = std::move(revealOnServer);
}

void FileTrackingPanel::onTabActivated()
{
    if (!impl_->attention.unseen())
        return;
    impl_->store.focusNewestConflict();
    impl_->attention.acknowledge();
}

Nui::ElementRenderer FileTrackingPanel::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    auto* const implementation = impl_.get();

    // clang-format off
    return div{
        class_ = "file-tracking-panel",
    }(
        div{
            class_ = "file-tracking-panel-layout",
        }(
            div{
                class_ = "file-tracking-panel-outliner",
            }(
                FileTracking::outliner(implementation->store, implementation->actions)
            ),
            div{
                class_ = "file-tracking-panel-detail",
            }(
                Nui::observe(implementation->store.selection()).generate([implementation]() -> Nui::ElementRenderer {
                    return implementation->renderDetail();
                })
            )
        )
    );
    // clang-format on
}
