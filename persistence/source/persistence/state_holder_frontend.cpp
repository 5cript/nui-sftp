#include <persistence/state_holder.hpp>
#include <log/log.hpp>

#include <nui/frontend/api/console.hpp>

namespace Persistence
{
    void StateHolder::load(std::function<void(std::optional<std::string> const& error, StateHolder&)> const& onLoad)
    {
        Nui::RpcClient::getRemoteCallableWithBackChannel(
            "StateHolder::load", [this, onLoad](Nui::val const& jsonStringOrError) {
                if (jsonStringOrError.hasOwnProperty("error"))
                {
                    onLoad(jsonStringOrError["error"].as<std::string>(), *this);
                    return;
                }

                try
                {
                    nlohmann::json::parse(jsonStringOrError.as<std::string>()).get_to(stateCache_);
                }
                catch (std::exception const& exc)
                {
                    Log::info("Failed to parse state from json: {}", exc.what());
                    onLoad(fmt::format("Failed to parse state from json: {}", exc.what()), *this);
                    return;
                }
                onLoad(std::nullopt, *this);
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
}