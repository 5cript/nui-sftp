#include <persistence/state_holder.hpp>
#include <log/log.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/json.hpp>

namespace Persistence
{
    LoadWarnings parseLoadWarnings(Nui::val const& warningsValue)
    {
        LoadWarnings warnings{};
        try
        {
            nlohmann::json::parse(Nui::JSON::stringify(warningsValue)).get_to(warnings);
        }
        catch (std::exception const& exc)
        {
            Log::error("Failed to parse state load warnings: {}", exc.what());
        }
        return warnings;
    }

    void StateHolder::load(
        std::function<void(std::optional<std::string> const& error, StateHolder&, LoadWarnings const& warnings)> const&
            onLoad
    )
    {
        Nui::RpcClient::getRemoteCallableWithBackChannel(
            "StateHolder::load", [this, onLoad](Nui::val const& objectWithErrorWarningState) {
                const auto warnings = objectWithErrorWarningState.hasOwnProperty("warnings")
                    ? parseLoadWarnings(objectWithErrorWarningState["warnings"])
                    : LoadWarnings{};

                if (objectWithErrorWarningState.hasOwnProperty("error"))
                {
                    onLoad(objectWithErrorWarningState["error"].as<std::string>(), *this, warnings);
                    return;
                }

                try
                {
                    nlohmann::json::parse(objectWithErrorWarningState["state"].as<std::string>()).get_to(stateCache_);
                }
                catch (std::exception const& exc)
                {
                    Log::info("Failed to parse state from json: {}", exc.what());
                    onLoad(exc.what(), *this, warnings);
                    return;
                }
                onLoad(std::nullopt, *this, warnings);
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
    void StateHolder::clearWarnings(std::function<void()> const& onComplete)
    {
        Nui::RpcClient::getRemoteCallableWithBackChannel("StateHolder::clearWarnings", [onComplete](Nui::val const&) {
            onComplete();
        })();
    }
    void StateHolder::loadModifySave(
        std::function<void(State&)> modifier,
        std::function<void(std::optional<std::string> const&)> onComplete
    )
    {
        load(
            [this, modifier = std::move(modifier), onComplete = std::move(onComplete)](
                std::optional<std::string> const& error, StateHolder&, LoadWarnings const&
            ) mutable
            {
                if (error)
                {
                    onComplete(error);
                    return;
                }
                modifier(stateCache_);
                save(std::move(onComplete));
            }
        );
    }

    void StateHolder::loadLanguageFiles(std::function<void(std::optional<nlohmann::json> const&)> const& onLoadComplete)
    {
        if (!onLoadComplete)
        {
            Log::error("StateHolder::loadLanguageFiles called with nullish onLoadComplete");
            return;
        }

        Log::info("StateHolder::loadLanguageFiles calling frontend.");
        Nui::RpcClient::getRemoteCallableWithBackChannel(
            "StateHolder::loadLanguageFiles",
            [onLoadComplete](Nui::val const& val) {
                Log::info("StateHolder::loadLanguageFiles got response, checking for error.");
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