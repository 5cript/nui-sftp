#include <frontend/session_components/file_tracking.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

struct FileTrackingPanel::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;

    Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog)
        : stateHolder(stateHolder)
        , events(events)
        , confirmDialog(confirmDialog)
    {}
};

FileTrackingPanel::FileTrackingPanel(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    ConfirmDialog* confirmDialog
)
    : impl_(std::make_unique<Implementation>(stateHolder, events, confirmDialog))
{}

Nui::ElementRenderer FileTrackingPanel::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    return div{}("Hi");
}