#include <frontend/settings/addressable_setting.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/id.hpp>
#include <nui/frontend/attributes/class.hpp>

Nui::ElementRenderer addressableSetting(std::string htmlId, Nui::ElementRenderer inner)
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    return div{
        id = std::move(htmlId),
        class_ = "addressable-setting",
    }(std::move(inner));
}