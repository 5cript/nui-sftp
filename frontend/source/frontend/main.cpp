#include <frontend/main_page.hpp>
#include <persistence/state_holder.hpp>
#include <log/log.hpp>

#include <nui/core.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/window.hpp>
#include <nui/frontend/api/console.hpp>

// #define NUI_ENABLE_LIVE_RELOAD
// #include <nui/frontend/live_styling.hpp>

#include <nui/frontend/dom/dom.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

#include <memory>

static std::unique_ptr<Persistence::StateHolder> persistence{};
static std::unique_ptr<FrontendEvents> frontendEvents{};
static std::unique_ptr<MainPage> mainPage{};
static std::unique_ptr<Nui::Dom::Dom> dom{};

bool tryLoad(std::shared_ptr<Nui::TimerHandle> const& setupWait)
{
    static int counter = 0;
    ++counter;
    const bool terminalUtilityAvailable = !Nui::val::global("terminalUtility").isUndefined();
    if (terminalUtilityAvailable || counter > 20)
    {
        if (setupWait->hasActiveTimer())
            setupWait->stop();

        if (counter > 20)
        {
            Log::error("Failed to load terminalUtility");
            Nui::WebApi::Console::log(Nui::val::global("terminalUtility"));
        }
        else
            Log::info("terminalUtility available.");

        persistence = std::make_unique<Persistence::StateHolder>();
        frontendEvents = std::make_unique<FrontendEvents>();
        mainPage = std::make_unique<MainPage>(persistence.get(), frontendEvents.get());
        dom = std::make_unique<Nui::Dom::Dom>();

        dom->setBody(Nui::Elements::body{}(mainPage->render()));
    }
    else
        Log::info("Waiting for terminalUtility to be available.");
    return terminalUtilityAvailable;
}

extern "C" void frontendMain()
{
    std::shared_ptr<Nui::TimerHandle> setupWait = std::make_shared<Nui::TimerHandle>();

    Log::setupFrontendLogger(
        [](std::chrono::system_clock::time_point const&, Log::Level, std::string const&) {},
        [once = false, setupWait](Log::Level) mutable
        {
            if (!once)
            {
                Nui::WebApi::Console::info("Log available");
                once = true;
                if (!tryLoad(setupWait))
                {
                    Nui::setInterval(
                        100,
                        [setupWait]()
                        {
                            tryLoad(setupWait);
                        },
                        [setupWait](Nui::TimerHandle&& t)
                        {
                            *setupWait = std::move(t);
                        }
                    );
                }
            }
        }
    );
}

EMSCRIPTEN_BINDINGS(nui_example_frontend)
{
    emscripten::function("main", &frontendMain);
}
#include <nui/frontend/bindings.hpp>