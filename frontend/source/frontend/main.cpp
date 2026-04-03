#include <frontend/main_page.hpp>
#include <frontend/theme_controller.hpp>
#include <utility/language.hpp>
#include <persistence/state_holder.hpp>
#include <log/log.hpp>

#include <nui/core.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/window.hpp>
#include <nui/frontend/api/console.hpp>

#include <nui/frontend/dom/dom.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

#include <memory>

std::unique_ptr<LanguageProvider> language{nullptr};

static std::unique_ptr<Persistence::StateHolder> persistence{};
static std::unique_ptr<FrontendEvents> frontendEvents{};
static std::unique_ptr<ThemeController> themeController{};
static std::unique_ptr<MainPage> mainPage{};
static std::unique_ptr<Nui::Dom::Dom> dom{};

namespace
{
    void printKeys(Nui::val obj)
    {
        Nui::val keys = Nui::val::global("Object").call<Nui::val>("keys", obj);
        int length = keys["length"].as<int>();
        for (int i = 0; i < length; ++i)
        {
            std::string key = keys[i].as<std::string>();
            Log::debug("Key: '{}'", key);
        }
    }
}

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
            return false;
        }
        else
            Log::info("terminalUtility available.");

        persistence = std::make_unique<Persistence::StateHolder>();
        persistence->load(
            [](std::optional<std::string> const&, Persistence::StateHolder&, std::optional<std::string> const&)
            {
                Log::info("State loaded, setting up frontend.");
                frontendEvents = std::make_unique<FrontendEvents>();
                frontendEvents->onLanguageChanged.value() = persistence->stateCache().localizationOptions.languageCode;
                frontendEvents->selectedTheme.value() = persistence->stateCache().uiOptions.theme;
                frontendEvents->darkLightMode.value() = persistence->stateCache().uiOptions.darkLightMode;

                themeController = std::make_unique<ThemeController>(*frontendEvents);

                persistence->loadLanguageFiles(
                    [](std::optional<nlohmann::json> lang)
                    {
                        Log::info("Language file loaded, continuing setup.");
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
                            mainPage =
                                std::make_unique<MainPage>(persistence.get(), frontendEvents.get(), *themeController);

                            Log::info("Creating DOM.");
                            dom = std::make_unique<Nui::Dom::Dom>();

                            Log::info("Rendering main page into DOM.");
                            dom->setBody(Nui::Elements::body{}(mainPage->render()));

                            Log::info("Calling setup completion function");
                            mainPage->onSetupComplete();

                            Log::debug("Dumping nui_rpc.backend and nui_rpc.frontend for debugging:");
                            auto nuiRpc = Nui::val::global("nui_rpc");
                            if (!nuiRpc.isUndefined() && nuiRpc.hasOwnProperty("backend"))
                            {
                                Log::debug("nui_rpc.backend:");
                                printKeys(nuiRpc["backend"]);
                                Log::debug("nui_rpc.frontend:");
                                printKeys(nuiRpc["frontend"]);
                            }
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
    Nui::RpcClient::awaitRpcAvailable(
        [](bool timeout)
        {
            if (timeout)
            {
                // Show load failure in dom body:
                dom = std::make_unique<Nui::Dom::Dom>();
                dom->setBody(
                    Nui::Elements::body{}(Nui::Elements::div{
                        Nui::Attributes::style = "color: red; font-size: 20px; text-align: center; margin-top: 20px;"
                    }("Failed to load application: RPC initialization timed out. Restart the application."))
                );
                return;
            }

            std::shared_ptr<Nui::TimerHandle> setupWait = std::make_shared<Nui::TimerHandle>();
            setupLogger(setupWait);
        },
        std::chrono::seconds(10)
    );
}

EMSCRIPTEN_BINDINGS(nui_example_frontend)
{
    emscripten::function("main", &frontendMain);
}
#include <nui/frontend/bindings.hpp>