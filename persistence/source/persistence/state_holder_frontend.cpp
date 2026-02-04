#include <persistence/state_holder.hpp>
#include <log/log.hpp>

#include <nui/frontend/api/console.hpp>

namespace Persistence
{
    void StateHolder::load(
        std::function<void(
            std::optional<std::string> const& error,
            StateHolder&,
            std::optional<std::string> const& warning
        )> const& onLoad
    )
    {
        Nui::RpcClient::getRemoteCallableWithBackChannel(
            "StateHolder::load", [this, onLoad](Nui::val const& objectWithErrorWarningState) {
                std::optional<std::string> warning{std::nullopt};
                if (objectWithErrorWarningState.hasOwnProperty("warning"))
                {
                    warning = objectWithErrorWarningState["warning"].as<std::string>();
                }

                if (objectWithErrorWarningState.hasOwnProperty("error"))
                {
                    onLoad(objectWithErrorWarningState["error"].as<std::string>(), *this, warning);
                    return;
                }

                try
                {
                    nlohmann::json::parse(objectWithErrorWarningState["state"].as<std::string>()).get_to(stateCache_);
                }
                catch (std::exception const& exc)
                {
                    Log::info("Failed to parse state from json: {}", exc.what());
                    onLoad(fmt::format("Failed to parse state from json: {}", exc.what()), *this, warning);
                    return;
                }
                onLoad(std::nullopt, *this, warning);
            })();
    }
    void StateHolder::save(std::function<void(std::optional<std::string> const& error)> const& onSaveComplete)
    {
        Nui::RpcClient::getRemoteCallableWithBackChannel("StateHolder::save", [onSaveComplete](Nui::val const& val) {
            if (val.hasOwnProperty("error"))
            {
                onSaveComplete(val["error"].as<std::string>());
                return;
            }
            onSaveComplete(std::nullopt);
        })(nlohmann::json(stateCache_).dump());
    }
    void StateHolder::loadLanguageFile(std::function<void(std::optional<nlohmann::json> const&)> const& onLoadComplete)
    {
        if (!onLoadComplete)
        {
            Log::error("StateHolder::loadLanguageFile called with nullish onLoadComplete");
            return;
        }

        Log::info("StateHolder::loadLanguageFile calling frontend.");
        Nui::RpcClient::getRemoteCallableWithBackChannel(
            "StateHolder::loadLanguageFile",
            [onLoadComplete](Nui::val const& val) {
                Log::info("StateHolder::loadLanguageFile got response, checking for error.");
                if (val.hasOwnProperty("error"))
                {
                    Log::error("Failed to load language file: {}", val["error"].as<std::string>());
                    onLoadComplete(std::nullopt);
                    return;
                }
                nlohmann::json j;
                try
                {
                    Nui::WebApi::Console::log("languageFile", val);
                    j = nlohmann::json::parse(val["jsonString"].as<std::string>());
                }
                catch (std::exception const& exc)
                {
                    Log::error("Failed to parse language file json: {}", exc.what());
                    onLoadComplete(std::nullopt);
                    return;
                }
                onLoadComplete(std::move(j));
            }
        )();
    }
}