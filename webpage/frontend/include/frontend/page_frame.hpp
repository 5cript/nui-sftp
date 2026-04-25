#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace NuiSftpPage
{
    /**
     * @brief Top-level page composition: nav, hero, sections, footer.
     */
    class PageFrame
    {
      public:
        PageFrame();
        ~PageFrame();
        PageFrame(PageFrame const&) = delete;
        PageFrame& operator=(PageFrame const&) = delete;
        PageFrame(PageFrame&&);
        PageFrame& operator=(PageFrame&&);

        Nui::ElementRenderer render();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
