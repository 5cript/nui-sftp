#pragma once

#include <frontend/session_components/operation_queue.hpp>
#include <frontend/dialog/confirm_dialog.hpp>

#include <frontend/session_components/operation_queue/operation_card_interface.hpp>

#include <ids/ids.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/svg.hpp>
#include <frontend/components/svg/scan.hpp>
#include <frontend/components/svg/download.hpp>
#include <frontend/components/svg/pause.hpp>
#include <frontend/components/svg/play.hpp>
#include <frontend/components/svg/refresh.hpp>
#include <frontend/components/svg/scan_animated.hpp>
#include <frontend/components/svg/scan.hpp>
#include <frontend/components/svg/upload.hpp>
#include <frontend/components/svg/delete.hpp>

#include <utility/convert_naming_convention.hpp>
#include <utility/visit_overloaded.hpp>
#include <utility/format_bytes.hpp>

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
            class_ = observe(state_).generate([this](){
                const auto state = state_.value();
                const auto isSubgridOperation = type_ == SharedData::OperationType::Scan || type_ == SharedData::OperationType::LocalScan || type_ == SharedData::OperationType::Delete;
                return fmt::format("opq-card {} {}", [&state]() -> std::string {
                    if (state == SharedData::OperationState::Completed)
                        return "opq-completed";
                    else if (state == SharedData::OperationState::Failed)
                        return "opq-failed";
                    else if (state == SharedData::OperationState::Canceled)
                        return "opq-canceled";
                    else if (state == SharedData::OperationState::PartialSuccess)
                        return "opq-partial-success";
                    else
                        return "";
                }(), !isSubgridOperation ? "opq-card-gridspan": "opq-card-subgrid");
            }),
        }(
            // Icon
            div{}(
                observe(state_),
                [this]() -> Nui::ElementRenderer {
                    if (type_ == SharedData::OperationType::Download || type_ == SharedData::OperationType::BulkDownload)
                        return Svgs::download();
                    else if (type_ == SharedData::OperationType::Upload || type_ == SharedData::OperationType::BulkUpload)
                        return Svgs::upload();
                    else if (type_ == SharedData::OperationType::Scan || type_ == SharedData::OperationType::LocalScan)
                    {
                        if (isCompletedState())
                            return Svgs::scan();
                        else
                            return Svgs::scanAnimated();
                    }
                    else if (type_ == SharedData::OperationType::Delete)
                    {
                        return Svgs::deleteIcon();
                    }
                    else if (type_ == SharedData::OperationType::CustomAction)
                    {
                        return Svgs::refresh();
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
                            transformedItems.push_back({.text = fmt::format("{}: {}", path.string(), entryError.toString())});
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
                        return "Error!";
                    return "?";
                })
            ),
            // Cancel / Remove
            button {
                class_ = "opq-btn opq-cancel-btn",
                onClick = [this](){
                    cancel();
                }
            }(
                observe(state_).generate([this]() -> std::string {
                    if (isCompletedState())
                        return "X";
                    return "X";
                })
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

    Ids::OperationId operationId() const
    {
        return operationId_;
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
                return "No Error";
            else
                error = fmt::format("{} entries failed:\n", failedEntries_.value().size());
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

    void failedEntries(std::vector<std::pair<std::filesystem::path, SharedData::OperationError>> entries) override
    {
        failedEntries_ = std::move(entries);
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
    Nui::Observed<std::vector<std::pair<std::filesystem::path, SharedData::OperationError>>> failedEntries_;
};