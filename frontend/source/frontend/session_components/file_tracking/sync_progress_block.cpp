#include <frontend/session_components/file_tracking/sync_progress_block.hpp>
#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>
#include <utility/language.hpp>

#include <script-nui-components/progress_bar.hpp>
#include <script-nui-components/progress_color.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief The progress texts that are not part of FileTrackingTexts.
         */
        struct ProgressTexts
        {
            /** @brief "{} of {} files" (done, done and pending). */
            std::string files{};
            /** @brief "{} waiting" (not started yet). */
            std::string waiting{};
            /** @brief "{} waiting for your answer". */
            std::string waitingForAnswer{};
            /** @brief "Up to date · last upload {}" (clock time). */
            std::string upToDateLastUpload{};
            /** @brief "Stopped · last upload {}" (clock time). */
            std::string stoppedLastUpload{};
            /** @brief "One change waits in Conflicts". */
            std::string conflictsWaitOne{};
            /** @brief "{} changes wait in Conflicts". */
            std::string conflictsWait{};
            /** @brief "{} of {} files up to date". */
            std::string filesUpToDate{};
        };

        ProgressTexts translatedProgressTexts()
        {
            return ProgressTexts{
                .files = language->get("fileTracking", "progressFiles"),
                .waiting = language->get("fileTracking", "progressWaiting"),
                .waitingForAnswer = language->get("fileTracking", "progressWaitingForAnswer"),
                .upToDateLastUpload = language->get("fileTracking", "progressUpToDateLastUpload"),
                .stoppedLastUpload = language->get("fileTracking", "progressStoppedLastUpload"),
                .conflictsWaitOne = language->get("fileTracking", "progressConflictsWaitOne"),
                .conflictsWait = language->get("fileTracking", "progressConflictsWait"),
                .filesUpToDate = language->get("fileTracking", "progressFilesUpToDate"),
            };
        }

        struct BarValues
        {
            std::uint64_t current{0u};
            std::uint64_t maximum{0u};
        };

        /**
         * @brief The line the block shows while nothing uploads.
         */
        struct Summary
        {
            std::string text{};
            /** @brief The value of data-tone: "conflicts", "failed" or empty. */
            std::string tone{};
            /** @brief The bar beside the line, none for no bar. */
            std::optional<BarValues> bar{};
        };

        /**
         * @brief Whether anything of the watch is on its way, other than changes that wait for a conflict answer.
         */
        bool isSyncing(SharedData::TrackedEntry const& entry, SharedData::WatchProgress const& progress)
        {
            if (entry.stop)
                return false;
            return entry.uploading || !progress.inFlight.empty() || progress.changesPending > progress.waitingForAnswer;
        }

        template <typename... ArgumentsT>
        std::string formatted(std::string const& pattern, ArgumentsT&&... arguments)
        {
            return fmt::format(fmt::runtime(pattern), std::forward<ArgumentsT>(arguments)...);
        }

        Summary stoppedSummary(
            ProgressTexts const& texts,
            FileTrackingTexts const& storeTexts,
            SharedData::WatchProgress const& progress
        )
        {
            if (!progress.lastSync)
                return {.text = storeTexts.statuses.stopped};
            return {.text = formatted(texts.stoppedLastUpload, localClockTime(progress.lastSync->endedAt))};
        }

        Summary answerSummary(ProgressTexts const& texts, SharedData::WatchProgress const& progress)
        {
            if (progress.changesDone > 0u)
            {
                const auto total = progress.changesDone + progress.changesPending;
                return {
                    .text = formatted(texts.filesUpToDate, progress.changesDone, total),
                    .tone = "conflicts",
                    .bar = BarValues{.current = progress.changesDone, .maximum = total},
                };
            }
            if (progress.waitingForAnswer == 1u)
                return {.text = texts.conflictsWaitOne, .tone = "conflicts"};
            return {.text = formatted(texts.conflictsWait, progress.waitingForAnswer), .tone = "conflicts"};
        }

        Summary failureSummary(
            FileTrackingTexts const& storeTexts,
            ProgressTexts const& texts,
            SharedData::WatchProgress const& progress,
            std::uint64_t failureCount
        )
        {
            if (!progress.lastSync || progress.lastSync->changes == 0u)
                return {.text = failedCountText(storeTexts, failureCount), .tone = "failed"};
            const auto changes = progress.lastSync->changes;
            const auto upToDate = changes - std::min(failureCount, changes);
            return {
                .text = formatted(texts.filesUpToDate, upToDate, changes),
                .tone = "failed",
                .bar = BarValues{.current = upToDate, .maximum = changes},
            };
        }

        Summary idleSummary(
            FileTrackingTexts const& storeTexts,
            ProgressTexts const& texts,
            SharedData::TrackedEntry const& entry,
            SharedData::WatchProgress const& progress,
            std::uint64_t failureCount
        )
        {
            if (entry.stop)
                return stoppedSummary(texts, storeTexts, progress);
            if (progress.waitingForAnswer > 0u)
                return answerSummary(texts, progress);
            if (failureCount > 0u)
                return failureSummary(storeTexts, texts, progress, failureCount);
            if (!progress.lastSync)
                return {.text = storeTexts.statuses.upToDate};
            return {
                .text = formatted(texts.upToDateLastUpload, localClockTime(progress.lastSync->endedAt)),
                .bar = BarValues{.current = progress.lastSync->changes, .maximum = progress.lastSync->changes},
            };
        }

        /**
         * @brief A counts bar that never changes. The summary renders it anew whenever its values change, so its
         *        observed values only need to live while it renders.
         */
        Nui::ElementRenderer fixedCountBar(BarValues values)
        {
            const auto current =
                std::make_shared<Nui::Observed<std::int64_t>>(static_cast<std::int64_t>(values.current));
            const auto maximum =
                std::make_shared<Nui::Observed<std::int64_t>>(static_cast<std::int64_t>(values.maximum));
            auto bar = ScriptNuiComponents::progressBar({
                .current = current,
                .maximum = maximum,
                .textMode = ScriptNuiComponents::ProgressBarTextMode::Counts,
                .colorMode = ScriptNuiComponents::ProgressColorMode::Hue,
                .zeroAsComplete = true,
                .shine = false,
            });
            return [bar = std::move(bar), current, maximum](Nui::Dom::Element& parent, Nui::Renderer const& renderer)
            {
                return bar(parent, renderer);
            };
        }

        /**
         * @brief One row of the files in flight: the path below the watched item and its bar. A free row keeps its
         *        place (data-free, hidden by CSS), so the rows below it do not move.
         */
        Nui::ElementRenderer inFlightRow(std::shared_ptr<InFlightFileItem> const& row)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            const auto nameText = [row]()
            {
                return Nui::observe(row->name).generate(
                    [row]()
                    {
                        return row->name.value();
                    }
                );
            };

            // clang-format off
            return div{
                class_ = "file-tracking-sync-line file-tracking-sync-file",
                "data-free"_attr = Nui::observe(row->path).generate(
                    [row]()
                    {
                        return row->path.value().empty();
                    }
                ),
            }(
                span{
                    class_ = "file-tracking-sync-file-name",
                    Nui::Attributes::title = nameText(),
                }(
                    text{nameText()}()
                ),
                ScriptNuiComponents::progressBar({
                    .current = row->bytes,
                    .maximum = row->totalBytes,
                    .textMode = ScriptNuiComponents::ProgressBarTextMode::Bytes,
                    .colorMode = ScriptNuiComponents::ProgressColorMode::Hue,
                    .attributes = {
                        "data-ft-progress-file"_attr = Nui::observe(row->path).generate(
                            [row]()
                            {
                                return row->path.value();
                            }
                        ),
                    },
                })
            );
            // clang-format on
        }

        /**
         * @brief "12 of 48 files · 34 waiting", plus the changes that wait for an answer.
         */
        Nui::ElementRenderer
        runningSummary(std::shared_ptr<ProgressTexts const> const& texts, std::shared_ptr<WatchItem> const& item)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::span;

            // clang-format off
            return span{class_ = "file-tracking-sync-summary"}(
                span{class_ = "file-tracking-sync-done"}(
                    text{Nui::observe(item->progress).generate(
                        [texts, item]()
                        {
                            auto const& progress = item->progress.value();
                            return formatted(
                                texts->files, progress.changesDone, progress.changesDone + progress.changesPending
                            );
                        }
                    )}()
                ),
                span{
                    class_ = "file-tracking-sync-waiting",
                    hidden = Nui::observe(item->progress).generate(
                        [item]()
                        {
                            return item->progress.value().changesWaiting == 0u;
                        }
                    ),
                }(
                    text{Nui::observe(item->progress).generate(
                        [texts, item]()
                        {
                            return formatted(texts->waiting, item->progress.value().changesWaiting);
                        }
                    )}()
                ),
                span{
                    class_ = "file-tracking-sync-answers",
                    hidden = Nui::observe(item->progress).generate(
                        [item]()
                        {
                            return item->progress.value().waitingForAnswer == 0u;
                        }
                    ),
                }(
                    text{Nui::observe(item->progress).generate(
                        [texts, item]()
                        {
                            return formatted(texts->waitingForAnswer, item->progress.value().waitingForAnswer);
                        }
                    )}()
                )
            );
            // clang-format on
        }

        /**
         * @brief What the block shows while uploading. It stays mounted (hidden otherwise), so its bars update in
         *        place and its file rows keep their order.
         */
        Nui::ElementRenderer
        runningPart(std::shared_ptr<ProgressTexts const> const& texts, std::shared_ptr<WatchItem> const& item)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            // clang-format off
            return div{
                class_ = "file-tracking-sync-running",
                hidden = Nui::observe(item->entry, item->progress).generate(
                    [item]()
                    {
                        return !isSyncing(item->entry.value(), item->progress.value());
                    }
                ),
            }(
                div{class_ = "file-tracking-sync-line file-tracking-sync-total"}(
                    runningSummary(texts, item),
                    ScriptNuiComponents::progressBar({
                        .current = item->bytesDone,
                        .maximum = item->bytesTotal,
                        .textMode = ScriptNuiComponents::ProgressBarTextMode::Bytes,
                        .colorMode = ScriptNuiComponents::ProgressColorMode::Hue,
                        .attributes = {
                            "data-ft-progress-total"_attr = true,
                        },
                    })
                ),
                div{class_ = "file-tracking-sync-files"}(
                    Nui::range(item->inFlight),
                    [item](long long, std::shared_ptr<InFlightFileItem> const& row) -> Nui::ElementRenderer
                    {
                        return inFlightRow(row);
                    }
                )
            );
            // clang-format on
        }

        /**
         * @brief The summary line and the bar of the last sync while nothing uploads, nothing while uploading.
         */
        Nui::ElementRenderer idlePart(
            TrackingStore const* store,
            std::shared_ptr<ProgressTexts const> const& texts,
            std::shared_ptr<WatchItem> const& item
        )
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // clang-format off
            return div{class_ = "file-tracking-sync-idle"}(
                Nui::observe(item->entry, item->progress, item->failureCount),
                [store, texts, item]() -> Nui::ElementRenderer
                {
                    auto const& entry = item->entry.value();
                    auto const& progress = item->progress.value();
                    if (isSyncing(entry, progress))
                        return Nui::nil();

                    const auto summary = idleSummary(
                        store->texts(), *texts, entry, progress, item->failureCount->value()
                    );
                    return div{
                        class_ = "file-tracking-sync-line",
                        "data-tone"_attr = summary.tone,
                    }(
                        span{
                            class_ = "file-tracking-sync-summary",
                            Nui::Attributes::title = summary.text,
                        }(
                            text{summary.text}()
                        ),
                        summary.bar ? fixedCountBar(*summary.bar) : Nui::nil()
                    );
                }
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer syncProgressBlock(
        TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        std::shared_ptr<WatchItem> const& item
    )
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if (!item)
            return Nui::nil();

        const auto texts = std::make_shared<ProgressTexts const>(translatedProgressTexts());

        // clang-format off
        return div{
            class_ = "file-tracking-sync",
            role = "group",
            "aria-label"_attr = language->get("fileTracking", "progressLabel"),
        }(
            runningPart(texts, item),
            idlePart(&store, texts, item)
        );
        // clang-format on
    }
}
