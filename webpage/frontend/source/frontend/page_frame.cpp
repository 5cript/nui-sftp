#include <frontend/page_frame.hpp>

#include <frontend/sections/footer.hpp>
#include <frontend/sections/features.hpp>
#include <frontend/sections/hero.hpp>
#include <frontend/sections/nav_bar.hpp>
#include <frontend/sections/open_source.hpp>
#include <frontend/sections/platforms.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

namespace NuiSftpPage
{
    struct PageFrame::Implementation
    {
        Sections::Features features;
    };

    PageFrame::PageFrame()
        : impl_{std::make_unique<Implementation>()}
    {}
    PageFrame::~PageFrame() = default;
    PageFrame::PageFrame(PageFrame&&) = default;
    PageFrame& PageFrame::operator=(PageFrame&&) = default;

    Nui::ElementRenderer PageFrame::render()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        return body{}(
            div{class_ = "field"}(
                img{
                    class_ = "field-glow-svg",
                    src = "assets/field-glow.svg",
                    alt = "",
                }()
            ),
            Sections::navBar(),
            Sections::hero(),
            impl_->features(),
            Sections::platforms(),
            Sections::openSource(),
            Sections::footer()
        );
    }
}
