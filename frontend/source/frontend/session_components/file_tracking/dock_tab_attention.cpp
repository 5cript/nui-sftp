#include <frontend/session_components/file_tracking/dock_tab_attention.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <nui/frontend/val.hpp>

#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace FileTracking
{
    namespace
    {
        std::string conflictsTitle(std::uint64_t count)
        {
            if (count == 0u)
                return {};
            if (count == 1u)
                return language->get("fileTracking", "attentionConflictsOne");
            return fmt::format(fmt::runtime(language->get("fileTracking", "attentionConflicts")), count);
        }

        std::string failedTitle(std::uint64_t count)
        {
            if (count == 0u)
                return {};
            if (count == 1u)
                return language->get("fileTracking", "attentionFailedOne");
            return fmt::format(fmt::runtime(language->get("fileTracking", "attentionFailed")), count);
        }

        std::chrono::milliseconds steadyNow()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            );
        }
    }

    struct DockTabAttention::Implementation
    {
        explicit Implementation(std::string panelId)
            : panelId{std::move(panelId)}
        {}

        std::string panelId;
        AttentionState state{};

        /**
         * @brief Hands @p attention to the dock tab of this session's panel.
         */
        void forward(Attention const& attention) const
        {
            auto manager = Nui::val::global("contentPanelManager");
            if (manager.isUndefined() || manager["setFileTrackingAttention"].isUndefined())
                return Log::warn("FileTracking: contentPanelManager.setFileTrackingAttention is missing");

            auto shown = Nui::val::object();
            shown.set("conflicts", static_cast<double>(attention.counts.conflicts));
            shown.set("failed", static_cast<double>(attention.counts.failed));
            shown.set("pulse", attention.pulse);
            shown.set("conflictsText", conflictsTitle(attention.counts.conflicts));
            shown.set("failedText", failedTitle(attention.counts.failed));
            manager.call<void>("setFileTrackingAttention", panelId, shown);
        }
    };

    DockTabAttention::DockTabAttention(std::string panelId)
        : impl_{std::make_unique<Implementation>(std::move(panelId))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_MOVE(DockTabAttention);

    void DockTabAttention::show(AttentionCounts const& counts)
    {
        if (const auto attention = impl_->state.update(counts, steadyNow()); attention)
            impl_->forward(*attention);
    }

    bool DockTabAttention::unseen() const
    {
        return impl_->state.unseen();
    }

    void DockTabAttention::acknowledge()
    {
        impl_->state.acknowledge();
    }

    void DockTabAttention::reset()
    {
        impl_->state.reset();
        impl_->forward(Attention{});
    }
}
