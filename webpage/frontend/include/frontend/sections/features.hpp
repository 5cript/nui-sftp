#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace NuiSftpPage::Sections
{
    /**
     * @brief Features section with the eight-slide product carousel.
     *
     * Owns the carousel and the shared page index so the slide state survives
     * across re-renders of the page frame.
     */
    class Features
    {
      public:
        Features();
        ~Features();
        Features(Features const&) = delete;
        Features& operator=(Features const&) = delete;
        Features(Features&&);
        Features& operator=(Features&&);

        Nui::ElementRenderer operator()();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
