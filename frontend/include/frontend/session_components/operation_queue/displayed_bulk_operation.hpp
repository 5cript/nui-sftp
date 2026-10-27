#pragma once

#include <frontend/session_components/operation_queue/in_flight_row_list.hpp>
#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <frontend/session_components/operation_queue/scan_phase_status.hpp>
#include <frontend/components/svg/arrow_right.hpp>

#include <script-nui-components/progress_bar.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <utility/format_bytes.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

struct DisplayedBulkOperation
    : public OperationCard<DisplayedBulkOperation>
    , public ScanningCard
{
  public:
    DisplayedBulkOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        SharedData::OperationType type,
        std::string sourceText,
        std::string destinationText,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction,
        bool scanning = false
    )
        : OperationCard{
              type,
              confirmDialog,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown),
              std::move(onCompleteAction)
          }
        , ScanningCard{scanning}
        , sourceText_{std::move(sourceText)}
        , destinationText_{std::move(destinationText)}
        , totalProgressBar_({
              .height = std::string{progressHeight},
              .minimum = 0,
              .maximum = 1,
              .texts = {.percent = "{:.0f}%"},
          })
    {}

    bool warrantsCancelConfirm() const override
    {
        return true;
    }

    void setProgress(SharedData::BulkProgress const& progress)
    {
        scanPhase_.end();
        std::vector<std::uint64_t> entryIndices{};
        entryIndices.reserve(progress.inFlight.size());
        for (auto const& file : progress.inFlight)
            entryIndices.push_back(file.entryIndex);
        inFlightRows_.update(
            entryIndices,
            [&progress](InFlightRow& row, std::optional<std::uint64_t> const& entryIndex)
            {
                if (!entryIndex)
                {
                    // Blank it: a freed row that kept its file would look like a stalled transfer.
                    if (!row.name.value().empty())
                        row.name = "";
                    row.bar.setProgress(0);
                    return;
                }
                const auto file =
                    std::ranges::find(progress.inFlight, *entryIndex, &SharedData::BulkFileProgress::entryIndex);
                if (file == progress.inFlight.end())
                    return;
                if (row.name.value() != file->file)
                    row.name = file->file;
                row.bar.setMaximum(static_cast<long long>(file->totalBytes));
                row.bar.setProgress(static_cast<long long>(file->bytes));
            }
        );

        // Without a byte total (empty files, unknown sizes) the byte bar would sit at 0 B
        // forever; fall back to counting entries so the bar still moves.
        if (progress.bytesTotal > 0)
        {
            totalProgressBar_.setTextMode(ScriptNuiComponents::ProgressBarTextMode::Bytes);
            totalProgressBar_.setProgress(static_cast<long long>(progress.bytesCurrent));
            totalProgressBar_.setMaximum(static_cast<long long>(progress.bytesTotal));
        }
        else
        {
            totalProgressBar_.setTextMode(ScriptNuiComponents::ProgressBarTextMode::Counts);
            totalProgressBar_.setProgress(static_cast<long long>(progress.fileCurrentIndex));
            totalProgressBar_.setMaximum(static_cast<long long>(progress.fileCount));
        }

        bytesPerSecond = progress.bytesPerSecond;

        fileCurrentIndex = progress.fileCurrentIndex;
        fileCount = progress.fileCount;

        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    Nui::ElementRenderer body() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{
            class_ = "opq-body opq-bulk"
        }(
            div{}(
                div{
                    style = "flex-grow: 1; overflow: hidden; min-width: 0;"
                }(
                    div{
                        class_ = "opq-transfer-route",
                        alt = fmt::format("{} → {}", sourceText_, destinationText_)
                    }(
                        span{
                            class_ = "opq-route-segment"
                        }(sourceText_),
                        span{
                            class_ = "opq-route-arrow"
                        }(Svgs::arrowRight()),
                        span{
                            class_ = "opq-route-segment"
                        }(destinationText_)
                    )
                ),
                span{
                    class_ = "opq-status-text opq-status-count"
                }(
                    observe(fileCurrentIndex, fileCount, state_, scanPhase_.scanning, scanPhase_.totalScanned),
                    [this](){
                        if (scanPhase_.scanning.value() && !isCompletedState())
                            return scanPhase_.text();
                        // On successful completion, pin the numerator to the
                        // total so a dropped or late terminal progress tick
                        // can't leave the card stuck at (N-1)/N.
                        const auto finished = state_.value() == SharedData::OperationState::Completed ||
                            state_.value() == SharedData::OperationState::PartialSuccess;
                        const auto current = (finished && fileCount.value() > 0)
                            ? fileCount.value()
                            : fileCurrentIndex.value();
                        return fmt::format("{}/{}", current, fileCount.value());
                    }
                ),
                span{
                    class_ = "opq-status-text opq-status-speed"
                }(
                    observe(bytesPerSecond),
                    [this](){
                        return fmt::format(
                            "{}/s",
                            Utility::formatBytes(
                                bytesPerSecond.value(), Utility::determineOrderOfMagnitude(bytesPerSecond.value())
                            )
                        );
                    }
                )
            ),
            div{
                class_ = "opq-bulk-total"
            }(
                totalProgressBar_()
            ),
            cancelNote(totalProgressBar_),
            div{
                class_ = "opq-bulk-files"
            }(
                Nui::range(inFlightRows_.rows()),
                [](long long, auto const& row) -> Nui::ElementRenderer {
                    return row->render();
                }
            )
        );
        // clang-format on
    }

    void state(SharedData::OperationState newState) override
    {
        OperationCard::state(newState);
        if (isCompletedState())
        {
            // Pin the total bar to its max and zero the speed. A dropped or late
            // terminal progress tick can otherwise leave the bar short of 100%
            // and the speed frozen at its last sample. Mirrors the count-pin in
            // body(). In-flight rows are gone by then; clear any leftovers.
            if (newState == SharedData::OperationState::Completed ||
                newState == SharedData::OperationState::PartialSuccess)
            {
                bytesPerSecond = 0;
                totalProgressBar_.setProgress(totalProgressBar_.maximum());
            }
            if (totalProgressBar_.textMode() == ScriptNuiComponents::ProgressBarTextMode::Percent)
                totalProgressBar_.setTextMode(ScriptNuiComponents::ProgressBarTextMode::Bytes);
            // A cancelled bar stays where it stopped, one that never started stays empty.
            if (newState != SharedData::OperationState::Canceled)
                totalProgressBar_.setZeroAsComplete();
            inFlightRows_.clear();
        }
    }

    std::pair<std::int64_t, std::int64_t> barPosition() const override
    {
        return {totalProgressBar_.progress(), totalProgressBar_.maximum()};
    }

    void barPosition(std::int64_t progress, std::int64_t maximum) override
    {
        scanPhase_.end();
        totalProgressBar_.setTextMode(ScriptNuiComponents::ProgressBarTextMode::Bytes);
        totalProgressBar_.setMaximum(maximum);
        totalProgressBar_.setProgress(progress);
    }

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        if (isCompletedState() || SharedData::isLocalOperation(type_))
            return std::nullopt;
        return Resumable::Bulk{.operationId = operationId()};
    }

  private:
    struct InFlightRow
    {
        Nui::Observed<std::string> name{""};
        ScriptNuiComponents::ProgressBar bar{{
            .height = std::string{progressHeight},
            .minimum = 0,
            .maximum = 1,
            .showMinimumAndMaximum = true,
            .byteMode = true,
        }};

        Nui::ElementRenderer render() const
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // clang-format off
            return div{
                class_ = "opq-bulk-file-row"
            }(
                span{
                    class_ = "opq-route-segment"
                }(
                    observe(name),
                    [this]() -> std::string {
                        return name.value();
                    }
                ),
                bar()
            );
            // clang-format on
        }
    };

  private:
    /** @brief Where the items come from, as the route of the card shows it. */
    std::string sourceText_;
    std::string destinationText_;
    Nui::Observed<std::uint64_t> fileCurrentIndex{0ull};
    Nui::Observed<std::uint64_t> fileCount{0ull};
    Nui::Observed<std::make_signed_t<std::size_t>> bytesPerSecond{0};

    /** @brief Shows "0%" until the first progress or the end, then bytes, or counts without a byte total. */
    ScriptNuiComponents::ProgressBar totalProgressBar_;
    // Mutable because Nui::range only binds to a non-const Observed and body() is const.
    mutable InFlightRowList<InFlightRow> inFlightRows_{};
};
