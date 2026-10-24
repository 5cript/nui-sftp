#pragma once

#include <frontend/session_components/file_tracking/attention_state.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>
#include <string>

namespace FileTracking
{
    /**
     * @brief Makes the File Tracking dock tab of a session count and glow. Feeds AttentionState with the steady clock
     *        and forwards only changes, through Nui::val::global("contentPanelManager")
     *        .setFileTrackingAttention(panelId, {conflicts, failed, pulse, conflictsText, failedText}): the counts as
     *        numbers, pulse true when the glow should play now, and the translated badge titles ("5 conflicts wait",
     *        "6 uploads failed", empty at 0).
     */
    class DockTabAttention
    {
      public:
        /**
         * @param panelId The session's layout id (sessionLayoutId), naming its dock panel in contentPanelManager.
         */
        explicit DockTabAttention(std::string panelId);
        ROAR_PIMPL_SPECIAL_FUNCTIONS_NO_MOVE(DockTabAttention);

        /**
         * @brief Takes the counts TrackingStore::onAttentionChanged reports and updates the tab when they or the pulse
         *        changed.
         */
        void show(AttentionCounts const& counts);

        /**
         * @return Whether conflicts arrived since the last @ref acknowledge, so a click on the tab should jump to the
         *         newest conflict.
         */
        bool unseen() const;

        /**
         * @brief The user jumped to the newest conflict.
         */
        void acknowledge();

        /**
         * @brief Forgets everything and shows zero counts, the session ended.
         */
        void reset();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
