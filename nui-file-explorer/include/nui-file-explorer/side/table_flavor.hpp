#pragma once

#include <nui-file-explorer/side/flavor_implementation.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/api/abort_controller.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <nui/frontend/val.hpp>

#include <vector>
#include <optional>
#include <memory>

namespace NuiFileExplorer
{
    class TableFlavor : public FlavorImplementation
    {
      public:
        TableFlavor(Side& impl, Side* otherSide);
        ~TableFlavor();
        Nui::ElementRenderer operator()() override;

      private:
        void setupResizeObserver();
        void initPixelWidths();

        static constexpr int columnCount = 4;
        static constexpr int resizeDragThreshold = 4;

        Nui::Observed<std::vector<std::optional<int>>> columnPixelWidths_;
        double lastContainerWidth_{0.0};
        Nui::val resizeObserver_;
        Nui::WebApi::AbortController mouseMoveAbort_;
        std::weak_ptr<Nui::Dom::BasicElement> tableRef_;
        Nui::val window_;
    };
}
