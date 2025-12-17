#include <nui-file-explorer/file_grid.hpp>
#include <nui-file-explorer/dropdown_menu.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    struct FileGrid::Implementation
    {
        Side leftSide;
        Side rightSide;

        Nui::Observed<bool> swapSides{false};

        std::function<void(std::string const&)> onError{};

        Implementation(
            Side::Settings const& settings,
            std::unique_ptr<ISideModel> leftModel,
            std::unique_ptr<ISideModel> rightModel)
            : leftSide{settings, std::move(leftModel)}
            , rightSide{settings, std::move(rightModel)}
        {}
    };

    FileGrid::FileGrid(
        Side::Settings const& settings,
        std::unique_ptr<ISideModel> leftModel,
        std::unique_ptr<ISideModel> rightModel)
        : impl_(std::make_unique<Implementation>(settings, std::move(leftModel), std::move(rightModel)))
    {}
    FileGrid::~FileGrid() = default;
    FileGrid::FileGrid(FileGrid&&) = default;
    FileGrid& FileGrid::operator=(FileGrid&&) = default;

    Side& FileGrid::leftSide()
    {
        return impl_->leftSide;
    }
    Side& FileGrid::rightSide()
    {
        return impl_->rightSide;
    }

    void FileGrid::onError(std::function<void(std::string const&)> const& callback)
    {
        impl_->onError = callback;
    }

    void FileGrid::onUneventfulClick()
    {
        impl_->leftSide.onUneventfulClick();
        impl_->rightSide.onUneventfulClick();
    }

    void FileGrid::closeMenus()
    {
        impl_->leftSide.closeMenus();
        impl_->rightSide.closeMenus();
    }

    void FileGrid::swapSides(bool doSwap)
    {
        impl_->swapSides = doSwap;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    Nui::ElementRenderer FileGrid::operator()(std::vector<Nui::Attribute>&& attributes)
    {
        using namespace std::string_literals;
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        attributes.emplace_back(onClick = [this](Nui::val) {
            onUneventfulClick();
        });
        attributes.emplace_back(class_ = "nui-file-grid");

        // clang-format off
        return div {
            std::move(attributes)
        }(
            observe(impl_->swapSides),
            [this]() -> Nui::ElementRenderer {
                auto* left = &impl_->leftSide;
                auto* right = &impl_->rightSide;

                if (impl_->swapSides.value())
                    std::swap(left, right);

                return div{
                    class_ = "nui-file-grid-content",
                    style = "width: 100%; flex-grow: 1; display: grid; grid-template-columns: 1fr 1fr; gap: 4px;",
                }(
                    (*left)(),
                    (*right)()
                );
            }
        );
        // clang-format on
    }
}