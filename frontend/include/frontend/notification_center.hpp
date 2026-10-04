#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/notifications.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief One message in the notification log; repeats of the same message share an entry.
 */
struct NotificationEntry
{
    std::uint64_t id{0};
    NotificationSeverity severity{NotificationSeverity::Info};
    std::string message{};
    std::int64_t firstSeenMilliseconds{0};
    std::int64_t lastSeenMilliseconds{0};
    std::size_t occurrences{1};
};

/**
 * @brief Warnings and errors that arrived while the log was closed; what the toolbar badge shows.
 */
struct NotificationAttention
{
    std::size_t unread{0};
    NotificationSeverity worst{NotificationSeverity::Info};
};

/**
 * @brief Owns the toast host and the in memory notification log of the session.
 *
 * Every notification is shown as a toast and added to the log, newest first. A repeat of a message
 * that is still fresh does not toast again, it counts up the existing entry instead, so a failure
 * reported by every open session shows once. Use the Notifications functions to send; this class is
 * for the main page, the toolbar and the log page.
 */
class NotificationCenter
{
  public:
    explicit NotificationCenter(FrontendEvents* events);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(NotificationCenter);

    /**
     * @brief Mounts the toast host. Call once, in the render tree.
     */
    Nui::ElementRenderer toasts();

    /**
     * @brief Toasts the message and adds it to the log.
     */
    void notify(NotificationSeverity severity, std::string message);

    /**
     * @brief The log, newest first, capped to the most recent entries.
     */
    Nui::Observed<std::vector<NotificationEntry>>& entries();

    /**
     * @brief Unread warnings and errors; reset when the log is opened.
     */
    Nui::Observed<NotificationAttention>& attention();

    /**
     * @brief The entry the log scrolls to and highlights, set when it is opened from a toast.
     */
    Nui::Observed<std::optional<std::uint64_t>>& focusedEntry();

    /**
     * @brief Opens the log page, optionally scrolled to one entry.
     */
    void open(std::optional<std::uint64_t> entryId = std::nullopt);

    /**
     * @brief Closes the log page.
     */
    void close();

    /**
     * @brief Empties the log.
     */
    void clear();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
