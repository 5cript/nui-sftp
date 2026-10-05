#pragma once

#include <string>

class NotificationCenter;

/**
 * @brief Severity of a notification; drives the color, the icon and whether it draws attention.
 */
enum class NotificationSeverity
{
    Info,
    Success,
    Warning,
    Error,
};

/**
 * @brief App wide entry point for messages the user has to see.
 *
 * Each message pops up as a toast, is kept in the notification log of the session and is written to
 * the application log. The main page owns the NotificationCenter and attaches it; everything else
 * just calls the functions. Messages sent before a center is attached are kept and shown on attach.
 */
namespace Notifications
{
    /**
     * @brief Attaches the center that receives every notification; nullptr detaches it.
     */
    void attach(NotificationCenter* center);

    /**
     * @brief Turns every error and critical log message into an error notification, the backend's
     *        included. Call once at startup.
     *
     * Use the functions below for messages written for the user; they are logged too, without
     * coming back through this route.
     */
    void routeLogErrors();

    /**
     * @brief Something the user triggered failed. Stays longer than the other severities.
     */
    void error(std::string message);

    /**
     * @brief Something worked, but not entirely as expected.
     */
    void warning(std::string message);

    /**
     * @brief Neutral information.
     */
    void info(std::string message);

    /**
     * @brief Confirmation that something worked.
     */
    void success(std::string message);
}
