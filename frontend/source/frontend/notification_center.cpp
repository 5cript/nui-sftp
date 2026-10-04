#include <frontend/notification_center.hpp>

#include <script-nui-components/toast.hpp>

#include <nui/event_system/event_context.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace
{
    constexpr std::int32_t errorDurationMilliseconds = 8000;

    /**
     * @brief How long a repeat of a message counts up its entry instead of toasting again.
     */
    constexpr std::int64_t repeatSuppressionMilliseconds = errorDurationMilliseconds;

    constexpr std::size_t maximumEntries = 200;

    std::int64_t nowMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    }

    ScriptNuiComponents::ToastSeverity toToastSeverity(NotificationSeverity severity)
    {
        switch (severity)
        {
            case NotificationSeverity::Info:
                return ScriptNuiComponents::ToastSeverity::Info;
            case NotificationSeverity::Success:
                return ScriptNuiComponents::ToastSeverity::Success;
            case NotificationSeverity::Warning:
                return ScriptNuiComponents::ToastSeverity::Warning;
            case NotificationSeverity::Error:
                return ScriptNuiComponents::ToastSeverity::Error;
        }
        return ScriptNuiComponents::ToastSeverity::Info;
    }

    bool drawsAttention(NotificationSeverity severity)
    {
        return severity == NotificationSeverity::Warning || severity == NotificationSeverity::Error;
    }
}

struct NotificationCenter::Implementation
{
    FrontendEvents* events;
    ScriptNuiComponents::Toast toast{{
        .style = ScriptNuiComponents::ToastStyle::Strip,
        .position = ScriptNuiComponents::ToastPosition::BottomCenter,
    }};
    Nui::Observed<std::vector<NotificationEntry>> entries{};
    Nui::Observed<NotificationAttention> attention{};
    Nui::Observed<std::optional<std::uint64_t>> focusedEntry{};
    std::uint64_t nextId{1};

    explicit Implementation(FrontendEvents* events)
        : events{events}
    {}

    void open(std::optional<std::uint64_t> entryId)
    {
        focusedEntry = entryId;
        attention = NotificationAttention{};
        events->notificationLogOpen = true;
    }

    /**
     * @brief Counts a repeat into the fresh entry for the message, if there is one.
     *
     * @return Whether such an entry existed.
     */
    bool countRepeat(NotificationSeverity severity, std::string const& message, std::int64_t now)
    {
        auto const& list = entries.value();
        const auto fresh = std::ranges::find_if(list, [&](NotificationEntry const& entry) {
            return entry.severity == severity && entry.message == message &&
                now - entry.lastSeenMilliseconds < repeatSuppressionMilliseconds;
        });
        if (fresh == list.end())
            return false;

        auto repeated = *fresh;
        ++repeated.occurrences;
        repeated.lastSeenMilliseconds = now;

        // The log is ordered by the latest occurrence, so the repeat moves to the top.
        const auto index = std::distance(list.begin(), fresh);
        if (index == 0)
        {
            entries[0] = std::move(repeated);
            return true;
        }
        entries.erase(entries.cbegin() + index);
        entries.insert(entries.cbegin(), std::move(repeated));
        return true;
    }

    void add(NotificationSeverity severity, std::string message, std::int64_t now)
    {
        const auto id = nextId++;
        entries.insert(
            entries.cbegin(),
            NotificationEntry{
                .id = id,
                .severity = severity,
                .message = message,
                .firstSeenMilliseconds = now,
                .lastSeenMilliseconds = now,
                .occurrences = 1,
            }
        );
        while (entries.value().size() > maximumEntries)
            entries.erase(std::prev(entries.cend()));

        toast.show({
            .message = std::move(message),
            .severity = toToastSeverity(severity),
            .durationMilliseconds =
                severity == NotificationSeverity::Error ? std::optional{errorDurationMilliseconds} : std::nullopt,
            .onClick =
                [this, id]() {
                    open(id);
                },
        });
    }
};

NotificationCenter::NotificationCenter(FrontendEvents* events)
    : impl_{std::make_unique<Implementation>(events)}
{}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(NotificationCenter);

Nui::ElementRenderer NotificationCenter::toasts()
{
    return impl_->toast();
}

void NotificationCenter::notify(NotificationSeverity severity, std::string message)
{
    const auto now = nowMilliseconds();
    if (!impl_->countRepeat(severity, message, now))
        impl_->add(severity, std::move(message), now);

    if (drawsAttention(severity) && !impl_->events->notificationLogOpen.value())
    {
        auto attention = impl_->attention.value();
        ++attention.unread;
        attention.worst = std::max(attention.worst, severity);
        impl_->attention = attention;
    }

    // Notifications mostly come from RPC replies and timers, outside of any event handler.
    Nui::globalEventContext.executeActiveEventsImmediately();
}

Nui::Observed<std::vector<NotificationEntry>>& NotificationCenter::entries()
{
    return impl_->entries;
}

Nui::Observed<NotificationAttention>& NotificationCenter::attention()
{
    return impl_->attention;
}

Nui::Observed<std::optional<std::uint64_t>>& NotificationCenter::focusedEntry()
{
    return impl_->focusedEntry;
}

void NotificationCenter::open(std::optional<std::uint64_t> entryId)
{
    impl_->open(entryId);
}

void NotificationCenter::close()
{
    impl_->focusedEntry = std::nullopt;
    impl_->events->notificationLogOpen = false;
}

void NotificationCenter::clear()
{
    impl_->entries.clear();
    impl_->focusedEntry = std::nullopt;
}
