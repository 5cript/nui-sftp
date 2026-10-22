#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/row_order.hpp>

#include <shared_data/file_tracking/server_changed_question.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/tracker_progress.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>
#include <log/log.hpp>

#include <script-nui-components/resizeable_table.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/style_variant.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/event_system/event_context.hpp>
#include <nui/rpc.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct FileTrackingPanel::Implementation
{
    /**
     * @brief What the row of one watch shows, shared with its cells.
     */
    struct Row
    {
        std::shared_ptr<Nui::Observed<std::string>> name{std::make_shared<Nui::Observed<std::string>>()};
        std::shared_ptr<Nui::Observed<std::string>> remotePath{std::make_shared<Nui::Observed<std::string>>()};
        std::shared_ptr<Nui::Observed<bool>> uploading{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> autoReupload{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> moveRemote{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> deleteRemote{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> checkRemote{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> downloadedCopy{std::make_shared<Nui::Observed<bool>>(false)};

        void show(SharedData::TrackedEntry const& entry)
        {
            *name = entry.name;
            *remotePath = entry.remotePath.value();
            *uploading = entry.uploading;
            *autoReupload = entry.options.autoReupload;
            *moveRemote = entry.options.moveRemoteOnLocalMove;
            *deleteRemote = entry.options.deleteRemoteOnLocalDelete;
            *checkRemote = entry.options.checkRemoteChanges;
            *downloadedCopy = entry.downloadedCopy;
        }
    };

    ConfirmDialog* confirmDialog;

    Ids::SessionId sessionId{};
    std::map<Ids::WatchId, Row> rows{};
    FileTracking::RowOrder<Ids::WatchId> rowOrder{};
    Nui::Observed<std::size_t> entryCount{std::size_t{0}};
    Nui::Observed<int> orphanedCount{-1};
    Nui::Observed<bool> cleanupRunning{false};
    Nui::Observed<SharedData::TrackerProgress> progress{};
    std::vector<Nui::RpcClient::AutoUnregister> listeners{};

    std::shared_ptr<ScriptNuiComponents::ResizableTable> table{std::make_shared<ScriptNuiComponents::ResizableTable>(
        ScriptNuiComponents::ResizableTable::HeaderRow{
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnName")},
                .initialWidth = 0,
                .resizeable = true,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnRemotePath")},
                .initialWidth = 0,
                .resizeable = true,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnAutoReupload")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnMoveRemote")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnDeleteRemote")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnCheckRemote")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnStatus")},
                .initialWidth = 100,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{},
                .initialWidth = 60,
                .resizeable = false,
            },
        },
        std::nullopt
    )};

    Utility::LifetimeToken lifetime{};

    explicit Implementation(ConfirmDialog* confirmDialog)
        : confirmDialog{confirmDialog}
    {}

    /**
     * @brief Shows what RPC replies and tracker events changed, they arrive outside of DOM event handlers.
     */
    static void syncView()
    {
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    /**
     * @brief Yes replaces the server's version, No keeps it, no button means another dialog took the question's place.
     */
    static SharedData::ServerChangedAnswer answerOf(std::optional<ConfirmDialog::Button> const& button)
    {
        if (!button)
            return SharedData::ServerChangedAnswer::Dismissed;
        return *button == ConfirmDialog::Button::Yes ? SharedData::ServerChangedAnswer::Replace
                                                     : SharedData::ServerChangedAnswer::Keep;
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

    void listen()
    {
        listeners.push_back(Nui::RpcClient::autoRegisterFunction(
            rpcName("onEntry"),
            lifetime.guarded(
                [this](SharedData::TrackedEntry const& entry)
                {
                    showEntry(entry);
                    syncView();
                }
            )
        ));
        listeners.push_back(Nui::RpcClient::autoRegisterFunction(
            rpcName("onEntryRemoved"),
            lifetime.guarded(
                [this](std::string const& watchId)
                {
                    removeEntry(Ids::makeWatchId(watchId));
                    syncView();
                }
            )
        ));
        listeners.push_back(Nui::RpcClient::autoRegisterFunction(
            rpcName("onQuestion"),
            lifetime.guarded(
                [this](SharedData::ServerChangedQuestion const& question)
                {
                    ask(question);
                }
            )
        ));
        listeners.push_back(Nui::RpcClient::autoRegisterFunction(
            rpcName("onProgress"),
            lifetime.guarded(
                [this](SharedData::TrackerProgress const& shown)
                {
                    progress = shown;
                    syncView();
                }
            )
        ));
        listeners.push_back(Nui::RpcClient::autoRegisterFunction(
            rpcName("onOrphanedCount"),
            lifetime.guarded(
                [this](std::uint64_t count)
                {
                    orphanedCount = static_cast<int>(count);
                    syncView();
                }
            )
        ));
    }

    void showEntry(SharedData::TrackedEntry const& entry)
    {
        auto [row, inserted] = rows.try_emplace(entry.watchId);
        row->second.show(entry);
        if (!inserted)
            return;
        table->addRow(buildRow(entry.watchId, row->second));
        rowOrder.add(entry.watchId);
        entryCount = rows.size();
    }

    void removeEntry(Ids::WatchId const& watchId)
    {
        rows.erase(watchId);
        if (const auto row = rowOrder.remove(watchId))
            table->remove(*row);
        entryCount = rows.size();
    }

    void ask(SharedData::ServerChangedQuestion const& question)
    {
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("fileTracking", "serverChangedHeader"),
            .text = fmt::format(
                fmt::runtime(language->get("fileTracking", "serverChangedText")), question.remotePath.value()
            ),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::No,
            .onClose = lifetime.guarded(
                [this, questionId = question.questionId](std::optional<ConfirmDialog::Button> button)
                {
                    forward("answer", questionId.value(), Utility::enumToString(answerOf(button)));
                }
            ),
        });
    }

    void refreshOrphanedCount()
    {
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::countOrphaned",
            lifetime.guarded(
                [this](Nui::val response)
                {
                    if (!succeeded(response))
                        return Log::error("FileTracking: countOrphaned failed: {}", errorOf(response).value_or(""));
                    orphanedCount = response["count"].as<int>();
                    syncView();
                }
            )
        );
    }

    void triggerCleanup()
    {
        cleanupRunning = true;
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::forceCleanup",
            lifetime.guarded(
                [this](Nui::val response)
                {
                    cleanupRunning = false;
                    if (succeeded(response))
                    {
                        Log::info(
                            "FileTracking: forceCleanup removed {} orphaned instance(s)", response["removed"].as<int>()
                        );
                        refreshOrphanedCount();
                    }
                    else
                        Log::error("FileTracking: forceCleanup failed: {}", errorOf(response).value_or(""));
                    syncView();
                }
            )
        );
    }

    /**
     * @brief How far the tracker is with the changes on their way, apart from the operation queue. Stays mounted, so
     *        the bar moves smoothly, and is hidden while nothing is on its way.
     */
    Nui::ElementRenderer renderProgress()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{
            class_ = Nui::observe(progress).generate([this]()
            {
                auto const& shown = progress.value();
                const bool active = shown.pending > 0 || shown.waitingForAnswer > 0;
                return std::string{active ? "file-tracking-progress" : "file-tracking-progress file-tracking-progress-idle"};
            }),
        }(
            div{class_ = "file-tracking-progress-track"}(
                div{
                    class_ = "file-tracking-progress-fill",
                    style = Nui::observe(progress).generate([this]()
                    {
                        auto const& shown = progress.value();
                        const auto total = shown.pending + shown.done;
                        return fmt::format("width: {}%", total == 0 ? 0 : shown.done * 100 / total);
                    }),
                }()
            ),
            span{class_ = "file-tracking-progress-text"}(
                Nui::observe(progress).generate([this]() -> Nui::ElementRenderer
                {
                    auto const& shown = progress.value();
                    return Nui::Elements::text{fmt::format(
                        fmt::runtime(language->get("fileTracking", "progressSyncing")),
                        shown.done,
                        shown.pending + shown.done
                    )}();
                })
            ),
            span{
                class_ = Nui::observe(progress).generate([this]()
                {
                    return std::string{progress.value().waitingForAnswer > 0
                        ? "file-tracking-progress-waiting"
                        : "file-tracking-progress-waiting file-tracking-progress-idle"};
                }),
            }(
                Nui::observe(progress).generate([this]() -> Nui::ElementRenderer
                {
                    return Nui::Elements::text{fmt::format(
                        fmt::runtime(language->get("fileTracking", "progressWaitingForAnswer")),
                        progress.value().waitingForAnswer
                    )}();
                })
            )
        );
        // clang-format on
    }

    Nui::ElementRenderer renderHeader()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return header{
            class_ = "file-tracking-header",
        }(
            div{class_ = "file-tracking-header-stats"}(
                span{class_ = "file-tracking-stat"}(
                    span{class_ = "file-tracking-stat-label"}(
                        Nui::Elements::text{std::string{language->get("fileTracking", "headerActive")}}()
                    ),
                    span{class_ = "file-tracking-stat-value"}(
                        Nui::observe(entryCount).generate([this]() -> Nui::ElementRenderer
                        {
                            return Nui::Elements::text{fmt::format("{}", entryCount.value())}();
                        })
                    )
                ),
                span{class_ = "file-tracking-stat"}(
                    span{class_ = "file-tracking-stat-label"}(
                        Nui::Elements::text{std::string{language->get("fileTracking", "headerOrphaned")}}()
                    ),
                    span{class_ = "file-tracking-stat-value file-tracking-stat-orphaned"}(
                        Nui::observe(orphanedCount).generate([this]() -> Nui::ElementRenderer
                        {
                            if (orphanedCount.value() < 0)
                                return Nui::Elements::text{"-"}();
                            return Nui::Elements::text{fmt::format("{}", orphanedCount.value())}();
                        })
                    )
                )
            ),
            renderProgress(),
            div{class_ = "file-tracking-header-actions"}(
                ScriptNuiComponents::button({
                    .text = Nui::observe(cleanupRunning).generate([](bool running) -> std::string
                    {
                        return std::string{language->get("fileTracking", running ? "headerCleanupRunning" : "headerCleanupButton")};
                    }),
                    .attributes =
                        {
                            Nui::Attributes::onClick = lifetime.guarded(
                                [this](Nui::val)
                                {
                                    if (!cleanupRunning.value())
                                        triggerCleanup();
                                }
                            ),
                            disabled = Nui::observe(cleanupRunning).generate([](bool running) { return running; }),
                        },
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                })
            )
        );
        // clang-format on
    }

    /**
     * @brief A cell with a switch that shows @p shown and forwards its changes as @p option of @p watchId.
     * @param downloadedCopy Whether the row is a temporary copy, which never moves or deletes the server's file: the
     *        switch of such an option is disabled then. Nothing for an option that applies to every row.
     */
    ScriptNuiComponents::ResizableTable::TableCell switchCell(
        Ids::WatchId const& watchId,
        std::shared_ptr<Nui::Observed<bool>> shown,
        SharedData::WatchOption option,
        std::shared_ptr<Nui::Observed<bool>> downloadedCopy = nullptr
    )
    {
        using namespace ScriptNuiComponents;
        return ResizableTable::TableCell{
            [this, watchId, shown = std::move(shown), option, downloadedCopy = std::move(downloadedCopy)](
                std::unique_ptr<ResizableTable::ISelfController>
            ) -> Nui::ElementRenderer
            {
                std::vector<Nui::Attribute> attributes{};
                if (downloadedCopy)
                {
                    attributes.push_back(Nui::Attributes::disabled = Nui::observe(*downloadedCopy).generate(
                                             [](bool isCopy)
                                             {
                                                 return isCopy;
                                             }
                                         ));
                    attributes.push_back(Nui::Attributes::title = Nui::observe(*downloadedCopy).generate(
                                             [](bool isCopy)
                                             {
                                                 return isCopy ? std::string{language->get(
                                                                     "fileTracking", "downloadedCopyOptionHint"
                                                                 )}
                                                               : std::string{};
                                             }
                                         ));
                }
                return ScriptNuiComponents::switch_({
                    .isChecked = shown,
                    .attributes = std::move(attributes),
                    .onChange = lifetime.guarded(
                        [this, watchId, option](bool checked, Nui::WebApi::MouseEvent const&)
                        {
                            forward("setOption", watchId.value(), Utility::enumToString(option), checked);
                        }
                    ),
                });
            },
        };
    }

    /**
     * @brief A cell that shows @p text and follows its changes.
     */
    static ScriptNuiComponents::ResizableTable::TableCell textCell(std::shared_ptr<Nui::Observed<std::string>> text)
    {
        using namespace ScriptNuiComponents;
        return ResizableTable::TableCell{
            [text = std::move(text)](std::unique_ptr<ResizableTable::ISelfController>) -> Nui::ElementRenderer
            {
                return Nui::Elements::div{
                    Nui::Attributes::style = "display:contents",
                }(Nui::observe(*text).generate(
                    [](std::string const& value) -> Nui::ElementRenderer
                    {
                        return Nui::Elements::text{value}();
                    }
                ));
            },
        };
    }

    ScriptNuiComponents::ResizableTable::TableRow buildRow(Ids::WatchId const& watchId, Row const& row)
    {
        using namespace ScriptNuiComponents;
        return ResizableTable::TableRow{
            textCell(row.name),
            textCell(row.remotePath),
            switchCell(watchId, row.autoReupload, SharedData::WatchOption::AutoReupload),
            switchCell(watchId, row.moveRemote, SharedData::WatchOption::MoveRemoteOnLocalMove, row.downloadedCopy),
            switchCell(
                watchId, row.deleteRemote, SharedData::WatchOption::DeleteRemoteOnLocalDelete, row.downloadedCopy
            ),
            switchCell(watchId, row.checkRemote, SharedData::WatchOption::CheckRemoteChanges),
            ResizableTable::TableCell{
                [uploading = row.uploading](std::unique_ptr<ResizableTable::ISelfController>) -> Nui::ElementRenderer
                {
                    return Nui::Elements::div{
                        Nui::Attributes::class_ = "file-tracking-status",
                        Nui::Attributes::style = "display:contents",
                    }(Nui::observe(*uploading)
                            .generate(
                                [](bool isUploading) -> Nui::ElementRenderer
                                {
                                    return Nui::Elements::text{std::string{language->get(
                                        "fileTracking", isUploading ? "statusUploading" : "statusWatching"
                                    )}}();
                                }
                            ));
                },
            },
            ResizableTable::TableCell{
                [this, watchId](std::unique_ptr<ResizableTable::ISelfController>) -> Nui::ElementRenderer
                {
                    return ScriptNuiComponents::button({
                        .text = std::string{language->get("fileTracking", "stopButton")},
                        .attributes =
                            {
                                Nui::Attributes::onClick = lifetime.guarded(
                                    [this, watchId](Nui::val)
                                    {
                                        forward("stop", watchId.value());
                                    }
                                ),
                            },
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    });
                },
            },
        };
    }
};

FileTrackingPanel::FileTrackingPanel(ConfirmDialog* confirmDialog)
    : impl_(std::make_unique<Implementation>(confirmDialog))
{}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FileTrackingPanel);

void FileTrackingPanel::activate(Ids::SessionId sessionId)
{
    deactivate();
    impl_->sessionId = std::move(sessionId);
    impl_->listen();
    impl_->refreshOrphanedCount();
}

void FileTrackingPanel::deactivate()
{
    impl_->listeners.clear();
    impl_->table->clear();
    impl_->rowOrder.clear();
    impl_->rows.clear();
    impl_->entryCount = std::size_t{0};
    impl_->sessionId = {};
    impl_->orphanedCount = -1;
    impl_->cleanupRunning = false;
    impl_->progress = SharedData::TrackerProgress{};
}

void FileTrackingPanel::open(
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

void FileTrackingPanel::checkUploadAndWatch(
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

void FileTrackingPanel::uploadAndWatch(
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

void FileTrackingPanel::discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
{
    impl_->forward("discard", instanceId.value(), localPath.toUtf8());
}

Nui::ElementRenderer FileTrackingPanel::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        class_ = "file-tracking-panel",
        style = "width: 100%; height: 100%;",
    }(
        impl_->renderHeader(),
        (*impl_->table)({})
    );
    // clang-format on
}
