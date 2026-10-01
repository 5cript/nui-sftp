#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <frontend/components/svg/arrow_right.hpp>

#include <shared_data/file_operations/bulk_progress.hpp>
#include <utility/format_bytes.hpp>

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

struct DisplayedBulkOperation : public OperationCard<DisplayedBulkOperation>
{
  public:
    DisplayedBulkOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        SharedData::OperationType type,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction
    )
        : OperationCard{
              type,
              confirmDialog,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown),
              std::move(onCompleteAction)
          }
        , localPath_{std::move(localPath)}
        , remotePath_{std::move(remotePath)}
        , totalProgressBar_({
              .height = std::string{progressHeight},
              .min = 0,
              .max = 1,
              .showMinMax = true,
              .byteMode = true,
          })
    {}

    bool warrantsCancelConfirm() const override
    {
        return true;
    }

    void setProgress(SharedData::BulkProgress const& progress)
    {
        // A row keeps its file until that file leaves the in-flight set; new files take the
        // first free row, so nothing shifts when an earlier file finishes.
        const auto inFlight = [&progress](std::uint64_t entryIndex)
        {
            return std::ranges::any_of(
                progress.inFlight,
                [entryIndex](SharedData::BulkFileProgress const& file)
                {
                    return file.entryIndex == entryIndex;
                }
            );
        };
        for (auto const& row : inFlightRows_.value())
        {
            if (row->entryIndex.has_value() && !inFlight(*row->entryIndex))
            {
                // Blank it: a freed row that kept its file would look like a stalled transfer.
                row->entryIndex.reset();
                row->name = "";
                row->bar.setProgress(0);
            }
        }
        for (auto const& file : progress.inFlight)
        {
            auto row = rowFor(file.entryIndex);
            if (!row)
                row = claimFreeRow(file.entryIndex);
            if (row->name.value() != file.file)
                row->name = file.file;
            row->bar.max(static_cast<long long>(file.totalBytes));
            row->bar.setProgress(static_cast<long long>(file.bytes));
        }
        trimFreeTailRows();

        // Without a byte total (empty files, unknown sizes) the byte bar would sit at 0 B
        // forever; fall back to counting entries so the bar still moves.
        if (progress.bytesTotal > 0)
        {
            totalProgressBar_.byteMode(true);
            totalProgressBar_.setProgress(static_cast<long long>(progress.bytesCurrent));
            totalProgressBar_.max(static_cast<long long>(progress.bytesTotal));
        }
        else
        {
            totalProgressBar_.byteMode(false);
            totalProgressBar_.setProgress(static_cast<long long>(progress.fileCurrentIndex));
            totalProgressBar_.max(static_cast<long long>(progress.fileCount));
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
                    [this]() -> Nui::ElementRenderer {
                        const auto& srcPath = type_ == SharedData::OperationType::BulkUpload ? localPath_ : remotePath_;
                        const auto& dstPath = type_ == SharedData::OperationType::BulkUpload ? remotePath_ : localPath_;
                        return div{
                            class_ = "opq-transfer-route",
                            alt = fmt::format("{} → {}", srcPath.generic_string(), dstPath.generic_string())
                        }(
                            span{
                                class_ = "opq-route-segment"
                            }(srcPath.generic_string()),
                            span{
                                class_ = "opq-route-arrow"
                            }(Svgs::arrowRight()),
                            span{
                                class_ = "opq-route-segment"
                            }(dstPath.generic_string())
                        );
                    }()
                ),
                span{
                    class_ = "opq-status-text opq-status-count"
                }(
                    observe(fileCurrentIndex, fileCount, state_),
                    [this](){
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
            div{
                class_ = "opq-bulk-files"
            }(
                Nui::range(inFlightRows_),
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
                totalProgressBar_.setProgress(totalProgressBar_.max());
            }
            totalProgressBar_.setZeroAsComplete();
            resizeInFlightRows(0);
        }
    }

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        if (isCompletedState())
            return std::nullopt;
        ResumableOp out;
        if (type_ == SharedData::OperationType::BulkDownload)
        {
            out.kind = ResumableOp::Kind::BulkDownload;
            out.src = remotePath_;
            out.dst = localPath_;
        }
        else
        {
            out.kind = ResumableOp::Kind::BulkUpload;
            out.src = localPath_;
            out.dst = remotePath_;
        }
        out.allowOverwrite = true;
        out.operationId = operationId();
        return out;
    }

  private:
    struct InFlightRow
    {
        std::optional<std::uint64_t> entryIndex{};
        Nui::Observed<std::string> name{""};
        Components::ProgressBar bar{{
            .height = std::string{progressHeight},
            .min = 0,
            .max = 1,
            .showMinMax = true,
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

    std::shared_ptr<InFlightRow> rowFor(std::uint64_t entryIndex) const
    {
        auto const& rows = inFlightRows_.value();
        const auto found = std::ranges::find_if(
            rows,
            [entryIndex](std::shared_ptr<InFlightRow> const& row)
            {
                return row->entryIndex == entryIndex;
            }
        );
        return found == rows.end() ? nullptr : *found;
    }

    /**
     * @brief Hands out the first row without a file, appending one when all are taken.
     */
    std::shared_ptr<InFlightRow> claimFreeRow(std::uint64_t entryIndex)
    {
        auto const& rows = inFlightRows_.value();
        auto found = std::ranges::find_if(
            rows,
            [](std::shared_ptr<InFlightRow> const& row)
            {
                return !row->entryIndex.has_value();
            }
        );
        if (found == rows.end())
        {
            resizeInFlightRows(rows.size() + 1);
            found = std::prev(inFlightRows_.value().end());
        }
        (*found)->entryIndex = entryIndex;
        return *found;
    }

    /**
     * @brief Drops free rows from the end so the list shrinks once files finish.
     */
    void trimFreeTailRows()
    {
        auto const& rows = inFlightRows_.value();
        auto keep = rows.size();
        while (keep > 0 && !rows[keep - 1]->entryIndex.has_value())
            --keep;
        resizeInFlightRows(keep);
    }

    /**
     * @brief Grows or shrinks the row list to @p wanted entries in a single insert or erase.
     */
    void resizeInFlightRows(std::size_t wanted)
    {
        auto const& rows = inFlightRows_.value();
        if (wanted > rows.size())
        {
            std::vector<std::shared_ptr<InFlightRow>> fresh(wanted - rows.size());
            std::ranges::generate(
                fresh,
                []
                {
                    return std::make_shared<InFlightRow>();
                }
            );
            inFlightRows_.insert(rows.end(), fresh.begin(), fresh.end());
        }
        else if (wanted < rows.size())
        {
            inFlightRows_.erase(rows.begin() + static_cast<std::ptrdiff_t>(wanted), rows.end());
        }
    }

  private:
    std::filesystem::path localPath_;
    std::filesystem::path remotePath_;
    Nui::Observed<std::uint64_t> fileCurrentIndex{0ull};
    Nui::Observed<std::uint64_t> fileCount{0ull};
    Nui::Observed<std::make_signed_t<std::size_t>> bytesPerSecond{0};

    Components::ProgressBar totalProgressBar_;
    // Mutable because Nui::range only binds to a non-const Observed and body() is const.
    mutable Nui::Observed<std::vector<std::shared_ptr<InFlightRow>>> inFlightRows_{};
};
