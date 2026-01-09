#pragma once

#include <nui/core.hpp>
#include <nui/rpc.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <persistence/state/state.hpp>

#include <functional>

namespace Persistence
{
    class StateHolder
    {
      public:
        StateHolder();
        ROAR_PIMPL_SPECIAL_FUNCTIONS(StateHolder);

        void load(
            std::function<void(
                std::optional<std::string> const& /*error*/,
                StateHolder&,
                std::optional<std::string> const& /*warning*/
            )> const& onLoad
        );
        void save(
            std::function<void(std::optional<std::string> const& error)> const& onSaveComplete =
                [](std::optional<std::string> const&) {}
        );

        State& stateCache();

#ifdef NUI_BACKEND
        void registerRpc(Nui::RpcHub& rpcHub);
        /**
         * @brief
         *
         * @param before
         * @return std::optional<std::string> A warning string if something was changed.
         */
        std::optional<std::string> dataFixer(nlohmann::json const& before);
#endif

      private:
        State stateCache_;
    };
} // namespace Persistence