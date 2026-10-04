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
 * Each message pops up as a toast and is kept in the notification log of the session. The main page
 * owns the NotificationCenter and attaches it; everything else just calls the functions. A message
 * sent while no center is attached is logged instead, so nothing is lost silently.
 */
namespace Notifications
{
    /**
     * @brief Attaches the center that receives every notification; nullptr detaches it.
     */
    void attach(NotificationCenter* center);

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
