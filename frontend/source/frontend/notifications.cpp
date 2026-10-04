#include <frontend/notifications.hpp>
#include <frontend/notification_center.hpp>

#include <log/log.hpp>

#include <utility>

namespace Notifications
{
    namespace
    {
        NotificationCenter* attachedCenter = nullptr;

        void notify(NotificationSeverity severity, std::string message)
        {
            if (attachedCenter == nullptr)
            {
                Log::warn("Notification without a notification center: {}", message);
                return;
            }
            attachedCenter->notify(severity, std::move(message));
        }
    }

    void attach(NotificationCenter* center)
    {
        attachedCenter = center;
    }

    void error(std::string message)
    {
        notify(NotificationSeverity::Error, std::move(message));
    }

    void warning(std::string message)
    {
        notify(NotificationSeverity::Warning, std::move(message));
    }

    void info(std::string message)
    {
        notify(NotificationSeverity::Info, std::move(message));
    }

    void success(std::string message)
    {
        notify(NotificationSeverity::Success, std::move(message));
    }
}
