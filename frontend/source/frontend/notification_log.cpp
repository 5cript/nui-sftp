#include <frontend/notification_log.hpp>
#include <frontend/components/icon_panel.hpp>
#include <frontend/svgs/decline.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/style_variant.hpp>

#include <ui5-sap-icons/icons/bell.hpp>
#include <ui5-sap-icons/icons/copy.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/error.hpp>
#include <ui5-sap-icons/icons/filter.hpp>
#include <ui5-sap-icons/icons/information.hpp>
#include <ui5-sap-icons/icons/list.hpp>
#include <ui5-sap-icons/icons/sys-enter.hpp>
#include <ui5-sap-icons/icons/warning.hpp>

#include <nui/event_system/listen.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <functional>
#include <string>

namespace
{
    enum class Filter
    {
        All,
        Errors,
        Warnings,
        Information,
    };

    bool matches(Filter filter, NotificationSeverity severity)
    {
        switch (filter)
        {
            case Filter::All:
                return true;
            case Filter::Errors:
                return severity == NotificationSeverity::Error;
            case Filter::Warnings:
                return severity == NotificationSeverity::Warning;
            case Filter::Information:
                return severity == NotificationSeverity::Info || severity == NotificationSeverity::Success;
        }
        return true;
    }

    std::string severityName(NotificationSeverity severity)
    {
        switch (severity)
        {
            case NotificationSeverity::Info:
                return "info";
            case NotificationSeverity::Success:
                return "success";
            case NotificationSeverity::Warning:
                return "warning";
            case NotificationSeverity::Error:
                return "error";
        }
        return "info";
    }

    Nui::ElementRenderer severityIcon(NotificationSeverity severity)
    {
        switch (severity)
        {
            case NotificationSeverity::Info:
                return Ui5Icons::information();
            case NotificationSeverity::Success:
                return Ui5Icons::sys_enter();
            case NotificationSeverity::Warning:
                return Ui5Icons::warning();
            case NotificationSeverity::Error:
                return Ui5Icons::error();
        }
        return Ui5Icons::information();
    }

    /**
     * @brief Local wall clock time as HH:MM:SS; the log only spans one session, so no date.
     */
    std::string clockTime(std::int64_t epochMilliseconds)
    {
        const auto date = Nui::val::global("Date").new_(Nui::val{static_cast<double>(epochMilliseconds)});
        return fmt::format(
            "{:02}:{:02}:{:02}", date.call<int>("getHours"), date.call<int>("getMinutes"), date.call<int>("getSeconds")
        );
    }

    std::string entryElementId(std::uint64_t id)
    {
        return fmt::format("notification-log-entry-{}", id);
    }

    /**
     * @brief One line per entry, in English on purpose: this is what ends up in bug reports.
     */
    std::string copyText(NotificationEntry const& entry)
    {
        std::string severity = severityName(entry.severity);
        std::ranges::transform(severity, severity.begin(), [](char character) {
            return static_cast<char>(character - 'a' + 'A');
        });
        if (entry.occurrences > 1)
        {
            return fmt::format(
                "[{}] {} {} (x{}, last {})",
                clockTime(entry.firstSeenMilliseconds),
                severity,
                entry.message,
                entry.occurrences,
                clockTime(entry.lastSeenMilliseconds)
            );
        }
        return fmt::format("[{}] {} {}", clockTime(entry.firstSeenMilliseconds), severity, entry.message);
    }

    void copyToClipboard(std::string const& textToCopy)
    {
        Nui::val::global("navigator")["clipboard"].call<Nui::val>("writeText", textToCopy);
    }

    void onNextFrame(std::function<void()> what)
    {
        Nui::val::global("requestAnimationFrame")(Nui::bind(
            [what = std::move(what)](Nui::val) {
                what();
            },
            std::placeholders::_1
        ));
    }
}

struct NotificationLog::Implementation
{
    FrontendEvents* events;
    NotificationCenter* center;
    Nui::Observed<Filter> filter{Filter::All};
    Nui::val pageElement{};
    Nui::ListenRemover<decltype(FrontendEvents::notificationLogOpen)> openListener{};
    Nui::ListenRemover<Nui::Observed<std::optional<std::uint64_t>>> focusListener{};

    Implementation(FrontendEvents* events, NotificationCenter* center)
        : events{events}
        , center{center}
    {}

    void scrollToFocusedEntry()
    {
        const auto entryId = center->focusedEntry().value();
        if (!entryId)
            return;
        const auto element =
            Nui::val::global("document").call<Nui::val>("getElementById", entryElementId(*entryId));
        if (element.isNull() || element.isUndefined())
            return;
        Nui::val options = Nui::val::object();
        options.set("block", "center");
        element.call<void>("scrollIntoView", options);
    }

    /**
     * @brief Focuses the page so Escape closes it, and brings the focused entry into view.
     */
    void revealOnNextFrame()
    {
        onNextFrame([this]() {
            if (!pageElement.isUndefined())
                pageElement.call<void>("focus");
            scrollToFocusedEntry();
        });
    }

    std::size_t count(Filter which) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(center->entries().value(), [which](auto const& entry) {
            return matches(which, entry.severity);
        }));
    }
};

NotificationLog::NotificationLog(FrontendEvents* events, NotificationCenter* center)
    : impl_{std::make_unique<Implementation>(events, center)}
{
    // The implementation stays put behind the pointer, the facade may move.
    auto* const implementation = impl_.get();
    impl_->openListener = Nui::smartListen(impl_->events->notificationLogOpen, [implementation](bool open) {
        if (open)
            implementation->revealOnNextFrame();
    });
    // A toast clicked while the page is already open only changes the focused entry.
    impl_->focusListener = Nui::smartListen(impl_->center->focusedEntry(), [implementation](auto const& entryId) {
        if (!entryId || !implementation->events->notificationLogOpen.value())
            return;
        if (implementation->filter.value() != Filter::All)
        {
            implementation->filter = Filter::All;
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
        implementation->revealOnNextFrame();
    });
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(NotificationLog);

Nui::ElementRenderer NotificationLog::header()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return div{class_ = "notification-log-header"}(
        iconPanel({
            .icon = Ui5Icons::bell(),
            .color = "var(--theme-color)",
            .withBorder = true,
        }),
        div{class_ = "title"}(language->get("notificationLog", "title")),
        Snc::button({
            .icon = GeneratedSvgs::decline(),
            .attributes = {
                onClick = [this]() {
                    impl_->center->close();
                },
            },
            .styleVariant = Snc::StyleVariant::Transparent,
        })
    );
    // clang-format on
}

Nui::ElementRenderer NotificationLog::side()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    const auto selector = [this](Filter which, char const* labelKey, Nui::ElementRenderer icon, std::string severity) {
        // clang-format off
        return div{
            class_ = Nui::observe(impl_->filter).generate([this, which]() {
                return impl_->filter.value() == which ? "notification-log-selector active" : "notification-log-selector";
            }),
            "data-severity"_attr = std::move(severity),
            onClick = [this, which]() {
                impl_->filter = which;
            },
        }(
            span{class_ = "notification-log-selector-icon"}(
                Nui::observe(impl_->filter),
                [this, which, icon = std::move(icon)]() -> Nui::ElementRenderer {
                    return iconPanel({
                        .icon = icon,
                        .color = impl_->filter.value() == which ? "var(--theme-color)" : "var(--background-color)",
                        .withBorder = true,
                    });
                }
            ),
            span{class_ = "notification-log-selector-label"}(language->get("notificationLog", labelKey)),
            span{class_ = "notification-log-count"}(
                Nui::observe(impl_->center->entries()).generate([this, which]() {
                    return std::to_string(impl_->count(which));
                })
            )
        );
        // clang-format on
    };

    // clang-format off
    return div{class_ = "notification-log-side"}(
        div{class_ = "notification-log-caption"}(
            Ui5Icons::filter(),
            span{}(language->get("notificationLog", "show"))
        ),
        selector(Filter::All, "all", Ui5Icons::list(), ""),
        selector(Filter::Errors, "errors", Ui5Icons::error(), "error"),
        selector(Filter::Warnings, "warnings", Ui5Icons::warning(), "warning"),
        selector(Filter::Information, "information", Ui5Icons::information(), "info")
    );
    // clang-format on
}

Nui::ElementRenderer NotificationLog::entryRow(NotificationEntry const& entry)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    const auto entryId = entry.id;
    const auto severity = entry.severity;

    const auto repeats = [&]() -> Nui::ElementRenderer {
        if (entry.occurrences < 2)
            return Nui::nil();
        return span{
            class_ = "notification-log-entry-repeats",
            Nui::Attributes::title = fmt::format(
                fmt::runtime(language->get("notificationLog", "repeatsTooltip")),
                entry.occurrences,
                clockTime(entry.lastSeenMilliseconds)
            ),
        }(fmt::format("×{}", entry.occurrences));
    };

    // clang-format off
    return div{
        id = entryElementId(entryId),
        class_ = Nui::observe(impl_->center->focusedEntry()).generate([this, entryId]() {
            return impl_->center->focusedEntry().value() == entryId
                ? "notification-log-entry notification-log-entry-focused"
                : "notification-log-entry";
        }),
        "data-severity"_attr = severityName(severity),
        style = Nui::observe(impl_->filter).generate([this, severity]() {
            return matches(impl_->filter.value(), severity) ? "" : "display: none;";
        }),
    }(
        span{class_ = "notification-log-entry-icon"}(severityIcon(severity)),
        div{class_ = "notification-log-entry-body"}(
            div{class_ = "notification-log-entry-message"}(entry.message),
            div{class_ = "notification-log-entry-meta"}(
                span{}(clockTime(entry.firstSeenMilliseconds)),
                repeats()
            )
        ),
        button{
            class_ = "notification-log-entry-copy",
            Nui::Attributes::title = language->get("notificationLog", "copy"),
            onClick = [text = copyText(entry)]() {
                copyToClipboard(text);
            },
        }(Ui5Icons::copy())
    );
    // clang-format on
}

Nui::ElementRenderer NotificationLog::main()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    const auto emptyLog = [this]() {
        return Nui::observe(impl_->center->entries()).generate([this]() -> std::optional<bool> {
            return impl_->center->entries().value().empty() ? std::optional<bool>{true} : std::nullopt;
        });
    };

    // clang-format off
    return div{class_ = "notification-log-main"}(
        div{class_ = "notification-log-actions"}(
            span{class_ = "notification-log-summary"}(
                Nui::observe(impl_->center->entries(), impl_->filter).generate([this]() {
                    const auto shown = impl_->count(impl_->filter.value());
                    if (shown == 1)
                        return language->get("notificationLog", "summaryOne");
                    return fmt::format(fmt::runtime(language->get("notificationLog", "summaryMany")), shown);
                })
            ),
            Snc::button({
                .text = language->get("notificationLog", "copyAll"),
                .icon = Ui5Icons::copy(),
                .attributes = {
                    disabled = emptyLog(),
                    onClick = [this]() {
                        std::string text;
                        for (auto const& entry : impl_->center->entries().value())
                        {
                            if (!matches(impl_->filter.value(), entry.severity))
                                continue;
                            text += copyText(entry);
                            text += '\n';
                        }
                        copyToClipboard(text);
                    },
                },
            }),
            Snc::button({
                .text = language->get("notificationLog", "clear"),
                .icon = Ui5Icons::delete_(),
                .attributes = {
                    disabled = emptyLog(),
                    onClick = [this]() {
                        impl_->center->clear();
                    },
                },
            })
        ),
        div{class_ = "notification-log-empty-host"}(
            Nui::observe(impl_->center->entries(), impl_->filter),
            [this]() -> Nui::ElementRenderer {
                if (impl_->center->entries().value().empty())
                    return div{class_ = "notification-log-empty"}(language->get("notificationLog", "empty"));
                if (impl_->count(impl_->filter.value()) == 0)
                    return div{class_ = "notification-log-empty"}(language->get("notificationLog", "emptyFiltered"));
                return Nui::nil();
            }
        ),
        div{class_ = "notification-log-list"}(
            Nui::range(impl_->center->entries()),
            [this](long long, NotificationEntry const& entry) -> Nui::ElementRenderer {
                return entryRow(entry);
            }
        )
    );
    // clang-format on
}

Nui::ElementRenderer NotificationLog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        class_ = "notification-log-background-blocker",
        style = Nui::observe(impl_->events->notificationLogOpen).generate([](bool isOpen) -> std::string {
            return isOpen ? "display: flex;" : "display: none;";
        }),
        tabIndex = "0",
        reference.onMaterialize([this](Nui::val element) {
            impl_->pageElement = std::move(element);
        }),
        onKeyDown = [this](Nui::val event) {
            if (event["key"].as<std::string>() == "Escape")
                impl_->center->close();
        },
    }(
        div{class_ = "notification-log-page"}(
            header(),
            div{class_ = "notification-log-content"}(
                side(),
                main()
            )
        )
    );
    // clang-format on
}
