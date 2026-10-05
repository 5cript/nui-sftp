#include <frontend/notifications.hpp>
#include <frontend/notification_center.hpp>

#include <log/log.hpp>

#include <nui/frontend/val.hpp>
#include <nui/frontend/utility/functions.hpp>

#include <cstddef>
#include <deque>
#include <string_view>
#include <utility>

namespace Notifications
{
    namespace
    {
        /**
         * @brief Notifications sent before the main page attached the center, e.g. startup errors.
         */
        constexpr std::size_t maximumPending = 50;

        constexpr std::string_view backendPrefix = "[MAIN] ";

        NotificationCenter* attachedCenter = nullptr;
        std::deque<std::pair<NotificationSeverity, std::string>> pending{};

        Log::Level toLogLevel(NotificationSeverity severity)
        {
            switch (severity)
            {
                case NotificationSeverity::Error:
                    return Log::Level::Error;
                case NotificationSeverity::Warning:
                    return Log::Level::Warning;
                case NotificationSeverity::Info:
                case NotificationSeverity::Success:
                    return Log::Level::Info;
            }
            return Log::Level::Info;
        }

        /**
         * @brief Hands the message to the center, or keeps it until one is attached.
         */
        void deliver(NotificationSeverity severity, std::string message)
        {
            if (attachedCenter == nullptr)
            {
                if (pending.size() == maximumPending)
                    pending.pop_front();
                pending.emplace_back(severity, std::move(message));
                return;
            }
            attachedCenter->notify(severity, std::move(message));
        }

        void notify(NotificationSeverity severity, std::string message)
        {
            {
                // Shown to the user already, so it must not come back through the log route.
                const Log::HooksSuppressed suppressed{};
                Log::log(toLogLevel(severity), "Notification: {}", message);
            }
            deliver(severity, std::move(message));
        }
    }

    void attach(NotificationCenter* center)
    {
        attachedCenter = center;
        if (attachedCenter == nullptr)
            return;
        auto waiting = std::exchange(pending, {});
        for (auto& [severity, message] : waiting)
            attachedCenter->notify(severity, std::move(message));
    }

    void routeLogErrors()
    {
        // Function local, so it is destroyed before the hook list it was added to.
        static const auto route = Log::addHook(Log::Level::Error, [](Log::Level, std::string const& message) {
            auto shown = message.starts_with(backendPrefix) ? message.substr(backendPrefix.size()) : message;
            // Errors are logged anywhere, also while rendering; notifying right there could re-enter
            // the event processing, so the notification is sent from a task of its own.
            Nui::val::global("setTimeout")(
                Nui::bind([shown = std::move(shown)]() {
                    deliver(NotificationSeverity::Error, shown);
                }),
                Nui::val{0}
            );
        });
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
