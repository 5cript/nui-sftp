#include <frontend/session_components/file_tracking/dock_tab_attention.hpp>

#include <utility>

namespace FileTracking
{
    struct DockTabAttention::Implementation
    {
        explicit Implementation(std::string panelId)
            : panelId{std::move(panelId)}
        {}

        std::string panelId;
        AttentionState state{};
    };

    DockTabAttention::DockTabAttention(std::string panelId)
        : impl_{std::make_unique<Implementation>(std::move(panelId))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_MOVE(DockTabAttention);

    void DockTabAttention::show([[maybe_unused]] AttentionCounts const& counts)
    {}

    bool DockTabAttention::unseen() const
    {
        return impl_->state.unseen();
    }

    void DockTabAttention::acknowledge()
    {}

    void DockTabAttention::reset()
    {}
}
