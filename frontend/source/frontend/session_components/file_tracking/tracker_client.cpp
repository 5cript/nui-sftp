#include <frontend/session_components/file_tracking/tracker_client.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>
#include <log/log.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/rpc.hpp>

#include <fmt/format.h>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTracking
{
    struct TrackerClient::Implementation
    {
        Implementation(TrackingStore& store, std::function<void(SharedData::EndedWatches const& ended)> onEnded)
            : store{&store}
            , onEnded{std::move(onEnded)}
        {}

        TrackingStore* store;
        std::function<void(SharedData::EndedWatches const& ended)> onEnded;
        Ids::SessionId sessionId{};
        std::vector<Nui::RpcClient::AutoUnregister> listeners{};
        Utility::LifetimeToken lifetime{};

        /**
         * @brief Shows what RPC replies and tracker events changed, they arrive outside of DOM event handlers.
         */
        static void syncView()
        {
            Nui::globalEventContext.executeActiveEventsImmediately();
        }

        static bool succeeded(Nui::val const& response)
        {
            return response.hasOwnProperty("success") && response["success"].as<bool>();
        }

        /**
         * @brief The translated error of a failed reply, nothing when it has none.
         */
        static std::optional<std::string> errorOf(Nui::val const& response)
        {
            if (!response.hasOwnProperty("error"))
                return std::nullopt;
            return language->translate(response["error"].as<std::string>());
        }

        std::string rpcName(std::string_view function) const
        {
            return fmt::format("FileTracker::{}::{}", sessionId.value(), function);
        }

        /**
         * @brief Forwards @p function with @p arguments to the tracker, a refusal is only logged.
         */
        template <typename... ArgumentsT>
        void forward(std::string_view function, ArgumentsT&&... arguments)
        {
            Nui::RpcClient::callWithBackChannel(
                rpcName(function),
                [function = std::string{function}](Nui::val response)
                {
                    if (!succeeded(response))
                        Log::error("FileTracker::{} failed: {}", function, errorOf(response).value_or(""));
                },
                std::forward<ArgumentsT>(arguments)...
            );
        }

        /**
         * @brief Registers @p event of the tracker, @p handler gets its payload and the view syncs once after it.
         */
        template <typename PayloadT, typename HandlerT>
        void listenTo(std::string_view event, HandlerT handler)
        {
            listeners.push_back(
                Nui::RpcClient::autoRegisterFunction(
                    rpcName(event),
                    lifetime.guarded(
                        [handler = std::move(handler)](PayloadT const& payload)
                        {
                            handler(payload);
                            syncView();
                        }
                    )
                )
            );
        }

        void listen()
        {
            listenTo<SharedData::TrackedEntry>(
                "onEntry",
                [this](SharedData::TrackedEntry const& entry)
                {
                    store->showEntry(entry);
                }
            );
            listenTo<std::string>(
                "onEntryRemoved",
                [this](std::string const& watchId)
                {
                    store->removeEntry(Ids::makeWatchId(watchId));
                }
            );
            listenTo<SharedData::EndedWatches>(
                "onEntriesEnded",
                [this](SharedData::EndedWatches const& ended)
                {
                    if (onEnded)
                        onEnded(ended);
                }
            );
            listenTo<SharedData::WatchConflict>(
                "onConflict",
                [this](SharedData::WatchConflict const& conflict)
                {
                    store->showConflict(conflict);
                }
            );
            listenTo<std::string>(
                "onConflictRemoved",
                [this](std::string const& conflictId)
                {
                    store->removeConflict(Ids::makeConflictId(conflictId));
                }
            );
            listenTo<SharedData::UploadFailure>(
                "onFailure",
                [this](SharedData::UploadFailure const& failure)
                {
                    store->showFailure(failure);
                }
            );
            listenTo<std::string>(
                "onFailureRemoved",
                [this](std::string const& failureId)
                {
                    store->removeFailure(Ids::makeFailureId(failureId));
                }
            );
            listenTo<SharedData::WatchProgress>(
                "onWatchProgress",
                [this](SharedData::WatchProgress const& progress)
                {
                    store->showWatchProgress(progress);
                }
            );
            listenTo<SharedData::OrphanedCopies>(
                "onOrphanedCopies",
                [this](SharedData::OrphanedCopies const& copies)
                {
                    store->showOrphanedCopies(copies);
                }
            );
        }
    };

    TrackerClient::TrackerClient(
        TrackingStore& store,
        std::function<void(SharedData::EndedWatches const& ended)> onEnded
    )
        : impl_{std::make_unique<Implementation>(store, std::move(onEnded))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_MOVE(TrackerClient);

    void TrackerClient::activate(Ids::SessionId sessionId)
    {
        deactivate();
        impl_->sessionId = std::move(sessionId);
        impl_->listen();
        listOrphanedCopies();
    }

    void TrackerClient::deactivate()
    {
        impl_->listeners.clear();
        impl_->store->clear();
        impl_->sessionId = {};
    }

    void TrackerClient::open(
        Paths::RemotePath const& remotePath,
        std::uint64_t sizeBytes,
        bool watch,
        std::function<void(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)> onOpened,
        std::function<void(std::string const& reason)> onRefused
    )
    {
        Nui::RpcClient::callWithBackChannel(
            impl_->rpcName("open"),
            impl_->lifetime.guarded(
                [remotePath, onOpened = std::move(onOpened), onRefused = std::move(onRefused)](Nui::val response)
                {
                    if (!Implementation::succeeded(response))
                    {
                        if (const auto error = Implementation::errorOf(response))
                            return onRefused(*error);
                        return Log::warn("FileTracking: '{}' was not downloaded, it is not opened", remotePath.value());
                    }
                    const auto localPath = Paths::LocalPath::fromUtf8(response["localPath"].as<std::string>());
                    if (!localPath)
                        return onRefused(localPath.error().toString());
                    onOpened(Ids::makeInstanceId(response["instanceId"].as<std::string>()), *localPath);
                }
            ),
            remotePath.value(),
            static_cast<double>(sizeBytes),
            watch
        );
    }

    void TrackerClient::checkUploadAndWatch(
        Paths::LocalPath const& item,
        Paths::RemotePath const& directory,
        std::function<void(Paths::LocalPath const& canonicalItem)> onWatchable,
        std::function<void(std::string const& reason)> onRefused
    )
    {
        Nui::RpcClient::callWithBackChannel(
            impl_->rpcName("checkUploadAndWatch"),
            impl_->lifetime.guarded(
                [onWatchable = std::move(onWatchable), onRefused = std::move(onRefused)](Nui::val response)
                {
                    if (!Implementation::succeeded(response))
                        return onRefused(Implementation::errorOf(response).value_or(""));
                    const auto canonicalItem = Paths::LocalPath::fromUtf8(response["item"].as<std::string>());
                    if (!canonicalItem)
                        return onRefused(canonicalItem.error().toString());
                    onWatchable(*canonicalItem);
                }
            ),
            item.toUtf8(),
            directory.value()
        );
    }

    void TrackerClient::uploadAndWatch(
        Paths::LocalPath const& item,
        Paths::RemotePath const& remotePath,
        std::function<void(std::string const& reason)> onRefused
    )
    {
        Nui::RpcClient::callWithBackChannel(
            impl_->rpcName("uploadAndWatch"),
            impl_->lifetime.guarded(
                [onRefused = std::move(onRefused)](Nui::val response)
                {
                    if (!Implementation::succeeded(response))
                        onRefused(Implementation::errorOf(response).value_or(""));
                }
            ),
            item.toUtf8(),
            remotePath.value()
        );
    }

    void TrackerClient::discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
    {
        impl_->forward("discard", instanceId.value(), localPath.toUtf8());
    }

    void TrackerClient::stop(Ids::WatchId const& watchId)
    {
        impl_->forward("stop", watchId.value());
    }

    void TrackerClient::setOption(Ids::WatchId const& watchId, SharedData::WatchOption option, bool enabled)
    {
        impl_->forward("setOption", watchId.value(), Utility::enumToString(option), enabled);
    }

    void TrackerClient::answerConflict(Ids::ConflictId const& conflictId, SharedData::ConflictResolution resolution)
    {
        impl_->forward("answerConflict", conflictId.value(), Utility::enumToString(resolution));
    }

    void TrackerClient::answerAllConflicts(Ids::WatchId const& watchId, SharedData::ConflictResolution resolution)
    {
        impl_->forward("answerAllConflicts", watchId.value(), Utility::enumToString(resolution));
    }

    void TrackerClient::retryFailure(Ids::FailureId const& failureId)
    {
        impl_->forward("retryFailure", failureId.value());
    }

    void TrackerClient::dismissFailure(Ids::FailureId const& failureId)
    {
        impl_->forward("dismissFailure", failureId.value());
    }

    void TrackerClient::retryAllFailures(Ids::WatchId const& watchId)
    {
        impl_->forward("retryAllFailures", watchId.value());
    }

    void TrackerClient::dismissAllFailures(Ids::WatchId const& watchId)
    {
        impl_->forward("dismissAllFailures", watchId.value());
    }

    void TrackerClient::removeStopped(Ids::WatchId const& watchId)
    {
        impl_->forward("removeStopped", watchId.value());
    }

    void TrackerClient::removeAllStopped()
    {
        impl_->forward("removeAllStopped");
    }

    void TrackerClient::listOrphanedCopies()
    {
        impl_->forward("listOrphanedCopies");
    }

    void TrackerClient::removeOrphanedCopy(
        Ids::InstanceId const& instanceId,
        std::function<void(std::string const& error)> onFailed
    )
    {
        Nui::RpcClient::callWithBackChannel(
            impl_->rpcName("removeOrphanedCopy"),
            impl_->lifetime.guarded(
                [onFailed = std::move(onFailed)](Nui::val response)
                {
                    if (Implementation::succeeded(response))
                        return;
                    if (onFailed)
                        onFailed(Implementation::errorOf(response).value_or(""));
                    Implementation::syncView();
                }
            ),
            instanceId.value()
        );
    }

    void TrackerClient::removeAllOrphanedCopies()
    {
        impl_->forward("removeAllOrphanedCopies");
    }
}
