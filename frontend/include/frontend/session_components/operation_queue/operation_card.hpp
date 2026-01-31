#pragma once

#include <frontend/session_components/operation_queue.hpp>

#include <frontend/session_components/operation_queue/operation_card_interface.hpp>

#include <ids/ids.hpp>

#include <fmt/format.h>

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

constexpr static std::string_view progressHeight{"15px"};

namespace Svgs = Components::Svg;

template <typename Derived>
class OperationCard : public OperationCardInterface
{
  public:
    explicit OperationCard(
        SharedData::OperationType type,
        Ids::OperationId operationId,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        bool invisible = false
    )
        : type_{type}
        , operationId_{std::move(operationId)}
        , doRemoveSelf_{std::move(doRemoveSelf)}
        , doDeletionCountdown_{std::move(doDeletionCountdown)}
        , invisible_{invisible}
    {}

    std::string formattedState() const
    {
        return Utility::splitByPascalCase(Utility::enumToString(state_.value())).joined(" ");
    }

    void state(SharedData::OperationState newState) override
    {
        state_ = newState;
    }

    SharedData::OperationState state() const override
    {
        return state_.value();
    }

    virtual std::string statusText() const override
    {
        return fmt::format("status: {}", formattedState());
    }

    SharedData::OperationType type() const override
    {
        return type_;
    }

    Nui::ElementRenderer operator()() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;
        using Nui::Elements::div;
        using Nui::Elements::span;

        if (invisible_)
            return Nui::nil();

        // clang-format off
        return section{
            class_ = observe(state_).generate([this](){
                const auto state = state_.value();
                if (state == SharedData::OperationState::Completed)
                    return "opq-card opq-completed opq-folded";
                else if (state == SharedData::OperationState::Failed)
                    return "opq-card opq-failed opq-folded";
                else if (state == SharedData::OperationState::Canceled)
                    return "opq-card opq-canceled opq-folded";
                else
                    return "opq-card";
            }),
        }(
            div {
                class_ = "opq-header",
            }(
                div {
                    class_ = "opq-type"
                }(
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
                div {
                    class_ = "opq-title"
                }(
                    div{}(static_cast<Derived const*>(this)->title()),
                    div {
                        class_ = "opq-muted",
                        Nui::Attributes::title = fmt::format("id: {}", operationId_.value())
                    }(
                        Nui::observe(state_),
                        [this]() -> std::string {
                            return static_cast<Derived const*>(this)->statusText();
                        }
                    )
                ),
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
                button {
                    class_ = "opq-btn opq-cancel-btn",
                    onClick = [this](){
                        cancel();
                    }
                }(
                    observe(state_).generate([this]() -> std::string {
                        if (isCompletedState())
                            return "Remove";
                        return "Cancel";
                    })
                )
            ),
            static_cast<Derived const*>(this)->body()
        );
        // clang-format on
    }

    bool isCompletedState() const override
    {
        const auto state = state_.value();
        return state == SharedData::OperationState::Completed || state == SharedData::OperationState::Failed ||
            state == SharedData::OperationState::Canceled;
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

        return div{style = "margin-top: 8px, font-size: 13px; color: var(--muted);"}(
            // TODO:
            "Elapsed: 0s"
        );
    }

    std::chrono::steady_clock::time_point completionTime() const override
    {
        return completionTime_.value();
    }

    void completionTime(std::chrono::steady_clock::time_point time) override
    {
        completionTime_ = time;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    std::chrono::steady_clock::time_point startTime() const
    {
        return startTime_;
    }

    auto bodyClass() const
    {
        using namespace Nui::Attributes;
        return class_ = observe(state_).generate(
                   [this]()
                   {
                       if (isCompletedState())
                           return "opq-body opq-collapsed";
                       return "opq-body";
                   }
               );
    }

  protected:
    std::chrono::steady_clock::time_point startTime_{std::chrono::steady_clock::now()};
    Nui::Observed<std::chrono::steady_clock::time_point> completionTime_{};
    Nui::Observed<SharedData::OperationState> state_{SharedData::OperationState::NotStarted};
    SharedData::OperationType type_;
    Ids::OperationId operationId_;
    std::function<void(OperationCard const& operation)> doRemoveSelf_;
    std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown_;
    bool invisible_{false};
};