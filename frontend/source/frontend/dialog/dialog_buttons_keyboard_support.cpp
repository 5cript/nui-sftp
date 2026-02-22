#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>

using namespace std::string_literals;

void dialogButtonContainerKeydown(Nui::WebApi::KeyboardEvent const& event)
{
    if (event.key() == "ArrowLeft" || event.key() == "ArrowUp" || event.key() == "ArrowRight" ||
        event.key() == "ArrowDown")
    {
        event.preventDefault();
        event.stopPropagation();
    }
    if (event.key() == "ArrowLeft" || event.key() == "ArrowUp")
    {
        auto button = event.currentTarget().call<Nui::val>("querySelector", "button:focus"s);

        if (button.isUndefined() || button.isNull())
            return;

        auto previous = button["previousElementSibling"];

        if (previous.isUndefined() || previous.isNull())
            previous = event.currentTarget().call<Nui::val>("querySelector", "button:last-child"s);

        if (previous.isUndefined() || previous.isNull())
            return;

        previous.call<void>("focus");
        return;
    }
    if (event.key() == "ArrowRight" || event.key() == "ArrowDown")
    {
        auto button = event.currentTarget().call<Nui::val>("querySelector", "button:focus"s);

        if (button.isUndefined() || button.isNull())
            return;

        auto next = button["nextElementSibling"];

        if (next.isUndefined() || next.isNull())
            next = event.currentTarget().call<Nui::val>("querySelector", "button"s);

        if (next.isUndefined() || next.isNull())
            return;

        next.call<void>("focus");
    }
}