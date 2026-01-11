#include <persistence/state_holder.hpp>

namespace Persistence
{
    StateHolder::StateHolder(std::filesystem::path programDirectory)
        : programDirectory_{std::move(programDirectory)}
    {}
    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(StateHolder);

    State& StateHolder::stateCache()
    {
        return stateCache_;
    }
} // namespace Persistence