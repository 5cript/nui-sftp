#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/notification_center.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

/**
 * @brief App wide page listing the notifications of the session, styled like the settings page.
 *
 * Opened from the toolbar or by clicking a toast, which scrolls to and highlights that entry.
 */
class NotificationLog
{
  public:
    NotificationLog(FrontendEvents* events, NotificationCenter* center);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(NotificationLog);

    Nui::ElementRenderer operator()();

  private:
    Nui::ElementRenderer header();
    Nui::ElementRenderer side();
    Nui::ElementRenderer main();
    Nui::ElementRenderer entryRow(NotificationEntry const& entry);

    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
