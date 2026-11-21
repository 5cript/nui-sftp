#include <persistence/state/session_layouts.hpp>

namespace Persistence
{
    namespace
    {
        template <typename LayoutsT>
        auto& layoutsOf(LayoutsT& layouts, TerminalEngineType type)
        {
            switch (type)
            {
                case TerminalEngineType::shell:
                    return layouts.shell;
                case TerminalEngineType::ftp:
                    return layouts.ftp;
                case TerminalEngineType::ssh:
                    return layouts.ssh;
            }
            return layouts.ssh;
        }
    }

    std::map<std::string, nlohmann::json>& SessionLayouts::of(TerminalEngineType type)
    {
        return layoutsOf(*this, type);
    }

    std::map<std::string, nlohmann::json> const& SessionLayouts::of(TerminalEngineType type) const
    {
        return layoutsOf(*this, type);
    }
}
