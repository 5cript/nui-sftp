#pragma once

#include <nui/core.hpp>
#include <nui/rpc.hpp>

#include <nlohmann/json.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <persistence/state/state.hpp>
#include <persistence/load_warning.hpp>

#include <functional>

namespace Persistence
{
    class StateHolder
    {
      public:
        StateHolder(std::filesystem::path programDirectory = {});
        ROAR_PIMPL_SPECIAL_FUNCTIONS(StateHolder);

        void load(
            std::function<void(
                std::optional<std::string> const& /*error*/,
                StateHolder&,
                LoadWarnings const& /*warnings*/
            )> const& onLoad
        );
        void save(
            std::function<void(std::optional<std::string> const& error)> const& onSaveComplete =
                [](std::optional<std::string> const&) {}
        );

        /**
         * @brief Loads the latest persisted state, applies a modifier to it, then saves it back.
         * This ensures no concurrent disk writes are lost.
         */
        void loadModifySave(
            std::function<void(State&)> modifier,
            std::function<void(std::optional<std::string> const&)> onComplete = [](std::optional<std::string> const&) {}
        );

        /**
         * @brief Loads all language files and assembles them into a single json object, which is passed to the
         * callback. The keys of the json object are the file names (without extension) of the language files, and the
         * values are the contents of the language files as json objects. Language files are expected to be in yaml
         * format and located in the "assets/languages" directory.
         *
         * @param onLoadComplete
         */
        void loadLanguageFiles(std::function<void(std::optional<nlohmann::json> const&)> const& onLoadComplete);

        State& stateCache();

        /**
         * @brief Clears any cached warnings from prior loads. To be called once the warning has been
         * presented to the user, so it is not shown again on subsequent loads.
         */
        void clearWarnings(std::function<void()> const& onComplete = [] {});

#ifdef NUI_BACKEND
        /**
         * @brief Loads a single language (expected format eg "en_US.yaml")
         *
         * @param filePath Path to the language file to load.
         * @param onLoadComplete Callback that is called when loading is complete. The json object is passed if loading
         * was successful, otherwise std::nullopt.
         */
        void loadLanguageFile(
            std::filesystem::path const& filePath,
            std::function<void(std::optional<nlohmann::json> const&)> const& onLoadComplete
        );

        void registerRpc(Nui::RpcHub& rpcHub);
        /**
         * @brief Adds missing defaults to the state cache and writes it back if anything changed.
         *
         * @param before The json as it was read from disk.
         * @return What was changed, empty if nothing was.
         */
        LoadWarnings dataFixer(nlohmann::json const& before);
#endif

      private:
        State stateCache_{};
        std::filesystem::path programDirectory_;
        LoadWarnings cachedWarnings_{};
    };
} // namespace Persistence