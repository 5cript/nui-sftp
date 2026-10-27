#pragma once

#include <frontend/session_components/operation_queue.hpp>
#include <frontend/dialog/confirm_dialog.hpp>

#include <frontend/session_components/operation_queue/operation_card_interface.hpp>

#include <ids/ids.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/svg.hpp>
#include <frontend/components/svg/scan.hpp>
#include <frontend/components/svg/pause.hpp>
#include <frontend/components/svg/play.hpp>
#include <frontend/components/svg/cancel.hpp>
#include <frontend/components/svg/delete.hpp>
#include <frontend/components/svg/scan_animated.hpp>
#include <frontend/components/icon_panel.hpp>

#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/upload.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/refresh.hpp>
#include <ui5-sap-icons/icons/edit.hpp>
#include <ui5-sap-icons/icons/drill-up.hpp>
#include <ui5-sap-icons/icons/attachment-zip-file.hpp>
#include <ui5-sap-icons/icons/copy.hpp>
#include <ui5-sap-icons/icons/move.hpp>
#include <ui5-sap-icons/icons/media-play.hpp>
#include <ui5-sap-icons/icons/slim-arrow-down.hpp>

#include <utility/language.hpp>
#include <utility/convert_naming_convention.hpp>
#include <utility/visit_overloaded.hpp>
#include <utility/format_bytes.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <string_view>

constexpr static std::string_view progressHeight{"11px"};

namespace Svgs = Components::Svg;

template <typename Derived>
class OperationCard : public OperationCardInterface
{
  public:
    explicit OperationCard(
        SharedData::OperationType type,
        ConfirmDialog& confirmDialog,
        Ids::OperationId operationId,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction
    )
        : type_{type}
        , confirmDialog_{&confirmDialog}
        , operationId_{std::move(operationId)}
        , doRemoveSelf_{std::move(doRemoveSelf)}
        , doDeletionCountdown_{std::move(doDeletionCountdown)}
        , onCompleteAction_{std::move(onCompleteAction)}
    {}

    std::string formattedState() const
    {
        return Utility::splitByPascalCase(Utility::enumToString(state_.value())).joined(" ");
    }

    void state(SharedData::OperationState newState) override
    {
        state_ = newState;
        if (isCompletedState())
        {
            completionTime_ = std::chrono::steady_clock::now();
            auto elem = cardElement_.lock();
            if (elem)
            {
                elem->val().call<void>("scrollIntoView");
            }
            if (onCompleteAction_)
            {
                onCompleteAction_();
                onCompleteAction_ = {};
            }
        }
    }

    SharedData::OperationState state() const override
    {
        return state_.value();
    }

    SharedData::OperationType type() const override
    {
        return type_;
    }

    void setError(SharedData::OperationError const& error) override
    {
        error_ = error;
    }

    Nui::ElementRenderer operator()() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{
            reference = [this](std::weak_ptr<Nui::Dom::BasicElement>&& element){
                cardElement_ = std::move(element);
            },
            class_ = observe(state_, keptPartialFiles_).generate([this](){
                const auto state = state_.value();
                const auto isSubgridOperation = type_ == SharedData::OperationType::Scan || type_ == SharedData::OperationType::LocalScan;
                return fmt::format("opq-card {} {}", [this, &state]() -> std::string {
                    if (state == SharedData::OperationState::Completed)
                        return "opq-completed";
                    else if (state == SharedData::OperationState::Failed)
                        return "opq-failed";
                    else if (state == SharedData::OperationState::Canceled)
                        return keptPartialFiles_.value() ? "opq-canceled opq-kept" : "opq-canceled";
                    else if (state == SharedData::OperationState::PartialSuccess)
                        return "opq-partial-success";
                    else if (state == SharedData::OperationState::Running)
                        return "opq-running";
                    else
                        return "";
                }(), !isSubgridOperation ? "opq-card-gridspan": "opq-card-subgrid");
            }),
            // Stable per-card identity: lets the queue's delegated drag handlers
            // resolve which card a dragstart/drop targets via closest("[data-op-id]").
            "data-op-id"_attr = operationId_.value(),
            // Always draggable; the dragstart handler at the list level rejects
            // non-paused drags via preventDefault — keeps gating in one place.
            "draggable"_attr = "true",
        }(
            // Kick-to-top button. Visibility gated purely by CSS using
            // .operation-queue[data-paused] selector — no per-card observed
            // state, so cost stays O(1) per card no matter the queue length.
            div{
                class_ = "op-kick-up",
                onClick = [this](Nui::val event){
                    // Stop the click from bubbling to the card root (which is
                    // also a drag source / potential parent click target).
                    event.call<void>("stopPropagation");
                    if (kickToTopHandler_)
                        kickToTopHandler_();
                },
                Nui::Attributes::title = language->get("operationQueue", "kickToTopTitle"),
            }(Ui5Icons::drill_up()),
            // Icon
            div{}(
                observe(state_),
                [this]() -> Nui::ElementRenderer {
                    if (type_ == SharedData::OperationType::Download || type_ == SharedData::OperationType::BulkDownload)
                        return Svgs::download();
                    else if (type_ == SharedData::OperationType::Upload || type_ == SharedData::OperationType::BulkUpload)
                        return Svgs::upload();
                    else if (type_ == SharedData::OperationType::ArchiveDownload ||
                        type_ == SharedData::OperationType::ArchiveUpload)
                        return Ui5Icons::attachment_zip_file();
                    else if (type_ == SharedData::OperationType::Scan || type_ == SharedData::OperationType::LocalScan)
                    {
                        if (isCompletedState())
                            return Svgs::scan();
                        else
                            return Svgs::scanAnimated();
                    }
                    else if (type_ == SharedData::OperationType::Rename)
                    {
                        return Ui5Icons::edit();
                    }
                    else if (type_ == SharedData::OperationType::Delete ||
                        type_ == SharedData::OperationType::BulkDelete ||
                        type_ == SharedData::OperationType::LocalDelete)
                    {
                        return Ui5Icons::delete_();
                    }
                    else if (type_ == SharedData::OperationType::LocalCopy)
                    {
                        return Ui5Icons::copy();
                    }
                    else if (type_ == SharedData::OperationType::LocalMove)
                    {
                        return Ui5Icons::move();
                    }
                    else if (type_ == SharedData::OperationType::CustomAction)
                    {
                        return Ui5Icons::refresh();
                    }
                    else
                        return div{}("UnknownType");
                }
            ),
            // Body
            static_cast<Derived const*>(this)->body(),
            // Clock
            div{
                class_ = "opq-clock"
            }(
                observe(completionTime_, doDeletionCountdown_).generate([this]() -> Nui::ElementRenderer {
                    if (isCompletedState() && *doDeletionCountdown_)
                    {
                        constexpr auto radius = 10;
                        constexpr auto circumference = 2 * 3.14159 * radius;
                        return svg::svg{
                            svga::viewBox = "0 0 24 24",
                            svga::fill = "none",
                            svga::width = "24",
                            svga::height = "24",
                        }(
                            svg::circle{
                                svga::r = "10",
                                svga::cx = "12",
                                svga::cy = "12",
                                svga::fill = "none",
                                svga::stroke = "white",
                                svga::strokeWidth = "2",
                                svga::strokeDasharray = std::to_string(circumference),
                                svga::strokeLinecap = "round",
                                svga::transform = "rotate(-90 12 12)",
                            }(
                                svg::animate{
                                    svga::attributeName = "stroke-dashoffset",
                                    svga::values = fmt::format("{};0", circumference),
                                    svga::dur = fmt::format("{}s", OperationQueue::autoRemoveTime.count()),
                                    svga::repeatCount = "1",
                                }()
                            )
                        );
                    }
                    return Nui::nil();
                })
            ),
            // Fail info
            button {
                class_ = "opq-btn opq-cancel-btn",
                onClick = [this](){
                    auto transformedItems = decltype(ConfirmDialog::OpenOptions::listItems){};
                    if (!failedEntries_.value().empty())
                    {
                        transformedItems.reserve(failedEntries_.value().size());
                        for (const auto& [path, entryError] : failedEntries_.value())
                            transformedItems.push_back({.text = fmt::format("{}: {}", path, entryError.toString())});
                    }

                    confirmDialog_->open(ConfirmDialog::OpenOptions{
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = "",
                        .text = makeError(),
                        .buttons = ConfirmDialog::Button::Ok,
                        .listItems = transformedItems,
                    });
                },
                style = observe(state_).generate([this](){
                    if (!isCompletedState() || (state_.value() != SharedData::OperationState::Failed && state_.value() != SharedData::OperationState::PartialSuccess))
                        return "display: none;";
                    return "";
                })
            }(
                observe(state_).generate([this]() -> std::string {
                    if (state_.value() == SharedData::OperationState::Failed || state_.value() == SharedData::OperationState::PartialSuccess)
                        return language->get("operationQueue", "errorButton");
                    return "?";
                })
            ),
            // Cancel / Remove, or continue and discard what a cancel kept
            div{
                class_ = "opq-actions"
            }(
                observe(state_, deletesPartialFilesOnCancel_, keptPartialFiles_),
                [this]() -> Nui::ElementRenderer {
                    return actions();
                }
            )
        );
        // clang-format on
    }

    bool isCompletedState() const override
    {
        const auto state = state_.value();
        return state == SharedData::OperationState::Completed || state == SharedData::OperationState::Failed ||
            state == SharedData::OperationState::Canceled || state == SharedData::OperationState::PartialSuccess;
    }

    void cancel() const
    {
        doRemoveSelf_(*this);
    }

    Ids::OperationId operationId() const override
    {
        return operationId_;
    }

    void setPartialFileHandlers(PartialFileHandlers handlers) override
    {
        partialFileHandlers_ = std::move(handlers);
    }

    void deletesPartialFilesOnCancel(std::optional<bool> deletes) override
    {
        deletesPartialFilesOnCancel_ = deletes;
    }

    std::optional<bool> deletesPartialFilesOnCancel() const override
    {
        return deletesPartialFilesOnCancel_.value();
    }

    void keptPartialFiles(bool kept) override
    {
        keptPartialFiles_ = kept;
    }

    bool keptPartialFiles() const override
    {
        return keptPartialFiles_.value();
    }

    Nui::ElementRenderer elapsedTime() const
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        return div{style = "margin-top: 8px, font-size: 13px; color: var(--color);"}(
            // TODO:
            "Elapsed: 0s"
        );
    }

    std::chrono::steady_clock::time_point completionTime() const override
    {
        return completionTime_.value();
    }

    std::chrono::steady_clock::time_point startTime() const
    {
        return startTime_;
    }

    std::string makeError() const
    {
        std::string error;
        if (!error_.value().has_value())
        {
            if (failedEntries_.value().empty())
                return language->get("operationQueue", "noError");
            else
                error = fmt::format(fmt::runtime(language->get("operationQueue", "entriesFailed")), failedEntries_.value().size());
        }
        else
            error = error_.value()->toString();
        return error;

        // std::vector<std::string> formattedIndividualEntries;
        // formattedIndividualEntries.reserve(failedEntries_.value().size());
        // for (const auto& [path, entryError] : failedEntries_.value())
        // {
        //     formattedIndividualEntries.push_back(fmt::format("{}: {}", path.string(), entryError.toString()));
        // }

        // // Format failed entries to list:
        // error = fmt::format("{}\n{}", error, fmt::join(formattedIndividualEntries, "\n"));
        // return error;
    }

    void failedEntries(std::vector<std::pair<std::string, SharedData::OperationError>> entries) override
    {
        failedEntries_ = std::move(entries);
    }

    void setKickToTopHandler(std::function<void()> handler) override
    {
        kickToTopHandler_ = std::move(handler);
    }

  private:
    std::string cancelMenuAnchorId() const
    {
        return fmt::format("opq-cancel-more-{}", operationId_.value());
    }

    /**
     * @brief The buttons at the end of the card: continue and discard for a transfer that kept its partial files, a
     *        quiet remove for an ended operation, cancel otherwise, with the cancel menu next to it for a transfer that
     *        deletes its partial files. The label of cancel hides in a narrow queue, see operation_queue.css.
     */
    Nui::ElementRenderer actions() const
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        const auto cancelContent = [](){
            return fragment(
                Svgs::cancel(),
                span{class_ = "opq-cancel-label"}(language->get("operationQueue", "cancel"))
            );
        };

        // clang-format off
        if (state_.value() == SharedData::OperationState::Canceled && keptPartialFiles_.value())
        {
            return fragment(
                span{class_ = "opq-kept-note"}(language->get("operationQueue", "partialFilesKept")),
                button{
                    class_ = "opq-btn opq-continue-btn",
                    Nui::Attributes::title = language->get("operationQueue", "continueKeptTitle"),
                    onClick = [this](){
                        if (partialFileHandlers_.continueTransfer)
                            partialFileHandlers_.continueTransfer();
                    }
                }(
                    Ui5Icons::media_play(),
                    span{}(language->get("operationQueue", "continueKept"))
                ),
                button{
                    class_ = "opq-btn opq-discard-btn",
                    Nui::Attributes::title = language->get("operationQueue", "discardKept"),
                    "aria-label"_attr = language->get("operationQueue", "discardKept"),
                    onClick = [this](){
                        if (partialFileHandlers_.discardPartialFiles)
                            partialFileHandlers_.discardPartialFiles();
                    }
                }(Ui5Icons::delete_())
            );
        }
        if (isCompletedState())
        {
            return button{
                class_ = "opq-dismiss-btn",
                Nui::Attributes::title = language->get("operationQueue", "removeFromList"),
                "aria-label"_attr = language->get("operationQueue", "removeFromList"),
                onClick = [this](){
                    cancel();
                }
            }(Svgs::deleteIcon());
        }
        if (deletesPartialFilesOnCancel_.value().value_or(false))
        {
            return div{
                class_ = "opq-cancel-split"
            }(
                button{
                    class_ = "opq-cancel-main",
                    Nui::Attributes::title = language->get("operationQueue", "cancelDeletingPartialFiles"),
                    onClick = [this](){
                        cancel();
                    }
                }(cancelContent()),
                button{
                    class_ = "opq-cancel-more",
                    id = cancelMenuAnchorId(),
                    Nui::Attributes::title = language->get("operationQueue", "moreCancelOptions"),
                    "aria-label"_attr = language->get("operationQueue", "moreCancelOptions"),
                    onClick = [this](Nui::val event){
                        event.call<void>("stopPropagation");
                        if (partialFileHandlers_.openCancelMenu)
                            partialFileHandlers_.openCancelMenu(cancelMenuAnchorId());
                    }
                }(Ui5Icons::slim_arrow_down())
            );
        }
        return button{
            class_ = "opq-cancel-main",
            Nui::Attributes::title = deletesPartialFilesOnCancel_.value() == false
                ? language->get("operationQueue", "cancelKeepsPartialFiles")
                : language->get("operationQueue", "cancel"),
            onClick = [this](){
                cancel();
            }
        }(cancelContent());
        // clang-format on
    }

  protected:
    mutable std::weak_ptr<Nui::Dom::BasicElement> cardElement_;
    std::chrono::steady_clock::time_point startTime_{std::chrono::steady_clock::now()};
    Nui::Observed<std::chrono::steady_clock::time_point> completionTime_{};
    Nui::Observed<SharedData::OperationState> state_{SharedData::OperationState::NotStarted};
    Nui::Observed<std::optional<SharedData::OperationError>> error_{std::nullopt};
    SharedData::OperationType type_;
    ConfirmDialog* confirmDialog_;
    Ids::OperationId operationId_;
    std::function<void(OperationCard const& operation)> doRemoveSelf_;
    std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown_;
    std::function<void()> onCompleteAction_;
    Nui::Observed<std::vector<std::pair<std::string, SharedData::OperationError>>> failedEntries_;
    std::function<void()> kickToTopHandler_;
    PartialFileHandlers partialFileHandlers_{};
    Nui::Observed<std::optional<bool>> deletesPartialFilesOnCancel_{std::nullopt};
    Nui::Observed<bool> keptPartialFiles_{false};
};