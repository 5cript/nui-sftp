#include <persistence/state/session_layouts.hpp>

namespace Persistence
{
    std::map<std::string, nlohmann::json>& SessionLayouts::of(TerminalEngineType type)
    {
        return type == TerminalEngineType::shell ? shell : ssh;
    }

    std::map<std::string, nlohmann::json> const& SessionLayouts::of(TerminalEngineType type) const
    {
        return type == TerminalEngineType::shell ? shell : ssh;
    }
}
