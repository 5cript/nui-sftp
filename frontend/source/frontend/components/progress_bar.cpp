#include <frontend/components/progress_bar.hpp>
#include <utility/format_bytes.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <fmt/format.h>

#include <algorithm>

// clang-format off
#ifdef NUI_INLINE
// @inline(css, scp-progress-bar)
.progress-bar {
    height: 30px;
    background-color: #606060;
    border-radius: 15px;
    overflow: hidden;
    position: relative;
    display: flex;
    justify-content: center;
    align-items: center;
    user-select: none;
}

.progress-bar > div:nth-child(1) {
    height: 100%;
    width: 0%;
    /*transition: width 0.3s linear, background-color 0.3s linear;*/
    background-color: #4CAF50;
    position: absolute;
    left: 0;
}

.progress-bar > div:nth-child(2) {
    position: relative;
    z-index: 1;
    font-weight: bold;
    color: #303030;
    text-shadow: 0 0 2px rgba(0, 0, 0, 0.5);
    text-align: center;
}

.progress-bar > div:nth-child(3) {
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: linear-gradient(to right, transparent, rgba(255, 255, 255, 0.5), transparent);
    animation: progress-shine 2s infinite;
}

.progress-bar > div:nth-child(3).hidden {
    display: none;
}

@media (prefers-reduced-motion: reduce) {
    .progress-bar > div:nth-child(3) {
        animation: none;
    }
}

@keyframes progress-shine {
    0% {
        transform: translateX(-100%);
    }

    100% {
        transform: translateX(100%);
    }
}
// @endinline
#endif
// clang-format on

namespace
{
    double percentageBetween(long long current, long long min, long long max)
    {
        // Ensure min is less than or equal to max
        if (min > max)
            std::swap(min, max);

        if (max == 0)
            return 0.;

        // Clamp current between min and max
        current = std::clamp(current, min, max);

        // Avoid division by zero if min == max
        if (min == max)
            return 100.;

        // Calculate percentage
        return 100. * (static_cast<double>(current - min) / static_cast<double>(max - min));
    }
}

namespace Components
{
    struct ProgressBar::Implementation : public ProgressBar::Settings
    {
        Nui::Observed<long long> progress{0};
        Nui::Observed<long long> maxObserved;
        Nui::Observed<std::string> text{"0%"};
        Nui::Observed<std::string> backgroundColor{"#a0a0a0"};
        Utility::OrderOfMagnitude magnitude;

        Implementation(Settings settings)
            : Settings{std::move(settings)}
            , maxObserved{max}
            , magnitude{Utility::determineOrderOfMagnitude(max)}
        {}
    };

    ProgressBar::ProgressBar(Settings settings)
        : impl_{std::make_unique<Implementation>(std::move(settings))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(ProgressBar);

    Nui::ElementRenderer ProgressBar::operator()() const
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using fmt::format;

        // clang-format off
    return div{
        class_ = "progress-bar",
        style = Style {
            "height"_style = impl_->height,
        },
    }(
        // bar fill
        div{
            style = Style{
                "width"_style = observe(impl_->progress, impl_->maxObserved).generate([this]() {
                    return format("{:.2f}%", percentageBetween(impl_->progress.value(), impl_->min, impl_->maxObserved.value()));
                }),
                "height"_style = impl_->height,
                "background-color"_style = impl_->backgroundColor
            },
        }(),
        // text
        div{}(impl_->text),
        // shine animation
        div{}()
    );
        // clang-format on
    }

    void ProgressBar::updateText()
    {
        impl_->text =
            [this, percent = percentageBetween(impl_->progress.value(), impl_->min, impl_->maxObserved.value())]()
        {
            if (impl_->showMinMax)
            {
                if (impl_->byteMode)
                {
                    return fmt::format(
                        "{} - {} ({:.1f}%)",
                        Utility::formatBytes(impl_->progress.value(), impl_->magnitude),
                        Utility::formatBytes(impl_->maxObserved.value(), impl_->magnitude),
                        percent
                    );
                }
                else
                    return fmt::format(
                        "{} / {} ({:.1f}%)", impl_->progress.value(), impl_->maxObserved.value(), percent
                    );
            }
            else
                return fmt::format("{:.1f}%", percent);
        }();
    }

    void ProgressBar::recalculate()
    {
        const auto current = impl_->progress.value();
        const auto percent = percentageBetween(current, impl_->min, impl_->maxObserved.value());
        const auto hue = current == impl_->maxObserved.value()
            ? 120.
            : std::max(5., 120. * std::pow(static_cast<double>(percent) / 100., 1.8));

        impl_->progress = current;
        impl_->backgroundColor = fmt::format("hsl({}, 40%, 45%)", static_cast<int>(hue));
        updateText();
    }

    void ProgressBar::setProgress(long long current)
    {
        impl_->progress = current;
        recalculate();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    long long ProgressBar::max() const
    {
        return impl_->maxObserved.value();
    }

    void ProgressBar::max(long long max)
    {
        if (max != impl_->maxObserved.value())
        {
            impl_->magnitude = Utility::determineOrderOfMagnitude(max);
            impl_->maxObserved = max;
            recalculate();
        }
    }
}