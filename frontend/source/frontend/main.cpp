#include <frontend/main_page.hpp>
#include <utility/language.hpp>
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

std::unique_ptr<LanguageProvider> language{nullptr};

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
        persistence->load(
            [](std::optional<std::string> const&, Persistence::StateHolder&, std::optional<std::string> const&)
            {
                frontendEvents = std::make_unique<FrontendEvents>();
                frontendEvents->onLanguageChanged = persistence->stateCache().localizationOptions.languageCode;

                persistence->loadLanguageFile(
                    [](std::optional<nlohmann::json> lang)
                    {
                        try
                        {
                            language = std::make_unique<LanguageProvider>(
                                frontendEvents.get(),
                                lang.value_or(
                                    []()
                                    {
                                        auto res = nlohmann::json::object();
                                        res["en_US"] = nlohmann::json::object();
                                        return res;
                                    }()
                                )
                            );

                            Log::info("Language file loaded.");
                            Log::info("Creating main page.");
                            mainPage = std::make_unique<MainPage>(persistence.get(), frontendEvents.get());

                            Log::info("Creating DOM.");
                            dom = std::make_unique<Nui::Dom::Dom>();

                            Log::info("Rendering main page into DOM.");
                            dom->setBody(Nui::Elements::body{}(mainPage->render()));

                            Log::info("Calling setup completion function");
                            mainPage->onSetupComplete();
                        }
                        catch (std::exception const& exc)
                        {
                            Log::error("Failed to continue setup after loading language file: {}", exc.what());
                        }
                    }
                );
            }
        );
    }
    else
        Log::info("Waiting for terminalUtility to be available.");
    return terminalUtilityAvailable;
}

void setupLogger(std::shared_ptr<Nui::TimerHandle> setupWait)
{
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

extern "C" void frontendMain()
{
    std::shared_ptr<Nui::TimerHandle> setupWait = std::make_shared<Nui::TimerHandle>();
    setupLogger(setupWait);
}

EMSCRIPTEN_BINDINGS(nui_example_frontend)
{
    emscripten::function("main", &frontendMain);
}
#include <nui/frontend/bindings.hpp>