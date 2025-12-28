#pragma once

#include <nui-file-explorer/side/flavor_implementation.hpp>

#include <nui/frontend/api/abort_controller.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/api/resize_observer.hpp>
#include <nui/frontend/element_renderer.hpp>

#include <chrono>

namespace NuiFileExplorer
{
    class Side;

    class IconFlavor : public FlavorImplementation
    {
      public:
        static constexpr std::chrono::milliseconds boxDragMinimumTimeToDifferentiateClick{150};

        IconFlavor(Side& impl);
        Nui::ElementRenderer operator()() override;

      private:
        void onBoxDragMouseMove(Nui::WebApi::MouseEvent event);
        void onBoxDragMouseUp(Nui::WebApi::MouseEvent event);
        void onBoxDragStart(Nui::WebApi::MouseEvent event);

      private:
        std::weak_ptr<Nui::Dom::BasicElement> gridRef_{};
        std::weak_ptr<Nui::Dom::BasicElement> selectBox_{};

        // Used for select rectangle:
        double startX_{0.};
        double startY_{0.};

        Nui::val selectionBox_{};
        Nui::WebApi::AbortController mouseMoveAbort_{};
        Nui::WebApi::AbortController mouseUpAbort_{};

        std::chrono::steady_clock::time_point mouseDownTime_{};

        // Used to prevent click events after drag box.
        bool shallClick_{true};

        std::unique_ptr<Nui::WebApi::ResizeObserver> gridLayoutObserver_{};
    };
}