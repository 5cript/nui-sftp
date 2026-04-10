#include <nui-file-explorer/file_grid.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/api/dom_rect.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    struct FileGrid::Implementation
    {
        Side leftSide;
        std::optional<Side> rightSide;

        Nui::Observed<bool> swapSides{false};
        std::function<void(std::string const&)> onError{};
        std::weak_ptr<Nui::Dom::BasicElement> grabber{};
        std::weak_ptr<Nui::Dom::BasicElement> grid{};
        std::weak_ptr<Nui::Dom::BasicElement> divider{};
        bool dragging{false};
        Nui::WebApi::AbortController mouseMoveAbort{};
        Nui::WebApi::AbortController mouseUpAbort{};

        bool checkReferencesForGrab()
        {
            auto grabElement = grabber.lock();
            auto gridElement = grid.lock();
            auto dividerElement = divider.lock();
            if (!grabElement || !gridElement || !dividerElement)
                return false;
            return true;
        }

        Implementation(
            SideSettings const& leftSettings,
            std::unique_ptr<ISideModel> leftModel,
            SideSettings const& rightSettings,
            std::unique_ptr<ISideModel> rightModel
        )
            : leftSide{leftSettings, std::move(leftModel)}
            , rightSide{std::make_optional<Side>(rightSettings, std::move(rightModel))}
        {}

        Implementation(SideSettings const& leftSettings, std::unique_ptr<ISideModel> leftModel)
            : leftSide{leftSettings, std::move(leftModel)}
            , rightSide{std::nullopt}
        {}
    };

    FileGrid::FileGrid(
        SideSettings const& leftSettings,
        SideSettings const& rightSettings,
        std::unique_ptr<ISideModel> leftModel,
        std::unique_ptr<ISideModel> rightModel
    )
        : impl_(
              std::make_unique<Implementation>(leftSettings, std::move(leftModel), rightSettings, std::move(rightModel))
          )
    {
        impl_->leftSide.initialize(&impl_->rightSide.value());
        impl_->rightSide->initialize(&impl_->leftSide);
    }
    FileGrid::FileGrid(SideSettings const& leftSettings, std::unique_ptr<ISideModel> leftModel)
        : impl_(std::make_unique<Implementation>(leftSettings, std::move(leftModel)))
    {
        impl_->leftSide.initialize(nullptr);
    }
    FileGrid::~FileGrid()
    {
        if (moveDetector_.wasMoved())
            return;

        impl_->mouseUpAbort.abort();
        impl_->mouseMoveAbort.abort();
    }
    FileGrid::FileGrid(FileGrid&&) = default;
    FileGrid& FileGrid::operator=(FileGrid&&) = default;

    Side& FileGrid::leftSide()
    {
        return impl_->leftSide;
    }
    Side* FileGrid::rightSide()
    {
        return impl_->rightSide ? &impl_->rightSide.value() : nullptr;
    }

    void FileGrid::onError(std::function<void(std::string const&)> const& callback)
    {
        impl_->onError = callback;
    }

    void FileGrid::onUneventfulClick()
    {
        impl_->leftSide.onUneventfulClick();
        if (impl_->rightSide)
            impl_->rightSide->onUneventfulClick();
    }

    void FileGrid::closeMenus()
    {
        impl_->leftSide.closeMenus();
        if (impl_->rightSide)
            impl_->rightSide->closeMenus();
    }

    void FileGrid::swapSides(bool doSwap)
    {
        impl_->swapSides = doSwap;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    ISideModel& FileGrid::leftModel()
    {
        return impl_->leftSide.model();
    }
    ISideModel* FileGrid::rightModel()
    {
        return impl_->rightSide ? &impl_->rightSide->model() : nullptr;
    }

    Nui::ElementRenderer FileGrid::operator()(std::vector<Nui::Attribute>&& attributes)
    {
        using namespace std::string_literals;
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        attributes.emplace_back(
            onClick =
                [this](Nui::val)
            {
                onUneventfulClick();
            }
        );
        attributes.emplace_back(class_ = "nui-file-grid nui-file-grid-background");

        impl_->mouseUpAbort.abort();
        impl_->mouseMoveAbort.abort();
        impl_->dragging = false;

        impl_->checkReferencesForGrab();

        // clang-format off
        return div {
            std::move(attributes)
        }(
            observe(impl_->swapSides),
            [this]() -> Nui::ElementRenderer {
                auto* left = &impl_->leftSide;
                auto* right = impl_->rightSide ? &impl_->rightSide.value() : nullptr;

                if (impl_->swapSides.value())
                    std::swap(left, right);

                return div{
                    class_ = "nui-file-grid-content",
                    style = [this](){
                        if (impl_->rightSide)
                            return "grid-template-columns: 1fr 1fr";
                        return "grid-template-columns: 1fr";
                    },
                    reference = [this](std::weak_ptr<Nui::Dom::BasicElement> elem) {
                        impl_->grid = elem;
                    }
                }(
                    (*left)(),
                    right ? (*right)() : Nui::nil(),
                    right ? div{
                        class_ = "nui-file-grid-divider",
                        reference = [this](std::weak_ptr<Nui::Dom::BasicElement> elem) {
                            impl_->divider = elem;
                        }
                    }() : Nui::nil(),
                    right ? div{
                        class_ = "nui-file-grid-grabber",
                        reference = [this](std::weak_ptr<Nui::Dom::BasicElement> elem) {
                            impl_->grabber = elem;

                            auto options = Nui::val::object();
                            impl_->mouseMoveAbort = {};
                            options.set("signal", impl_->mouseMoveAbort.signal().val());

                            Nui::val::global("window").call<void>(
                                "addEventListener",
                                "mousemove"s,
                                Nui::bind(
                                    [this](Nui::val rawEvent)
                                    {
                                        Nui::WebApi::MouseEvent event{std::move(rawEvent)};
                                        if (!impl_->checkReferencesForGrab())
                                            return;

                                        if (!impl_->dragging)
                                            return;

                                        auto grid = impl_->grid.lock();
                                        auto grabber = impl_->grabber.lock();
                                        auto divider = impl_->divider.lock();

                                        const auto rect = Nui::WebApi::DomRect{grid->val().call<Nui::val>("getBoundingClientRect")};
                                        const auto x = event.clientX() - rect.left();
                                        const auto pct = std::max(10., std::min(90., (x / rect.width()) * 100.));

                                        grid->val()["style"].set(
                                            "gridTemplateColumns",
                                            fmt::format("{}% {}% ", pct, 100. - pct)
                                        );
                                        grabber->val()["style"].set(
                                            "left",
                                            fmt::format("calc({}% - {}px)", pct, grabber->val()["offsetWidth"].as<int>() / 2)
                                        );
                                        divider->val()["style"].set(
                                            "left",
                                            fmt::format("calc({}% - {}px)", pct, 1)
                                        );
                                    },
                                    std::placeholders::_1
                                ),
                                options
                            );

                            auto options2 = Nui::val::object();
                            impl_->mouseUpAbort = {};
                            options2.set("signal", impl_->mouseUpAbort.signal().val());

                            Nui::val::global("window").call<void>(
                                "addEventListener",
                                "mouseup"s,
                                Nui::bind(
                                    [this](Nui::val)
                                    {
                                        if (!impl_->checkReferencesForGrab())
                                            return;
                                        impl_->dragging = false;
                                    },
                                    std::placeholders::_1
                                ),
                                options2
                            );
                        },
                        !("mousedown"_event = [this](Nui::WebApi::MouseEvent) {
                            impl_->dragging = true;
                        })
                    }() : Nui::nil()
                );
            }
        );
        // clang-format on
    }
}