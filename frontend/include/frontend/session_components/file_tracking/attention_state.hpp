#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace FileTracking
{
    /**
     * @brief What waits for the user across every watch of a session.
     */
    struct AttentionCounts
    {
        std::uint64_t conflicts{0u};
        std::uint64_t failed{0u};

        friend bool operator==(AttentionCounts const&, AttentionCounts const&) = default;
    };

    /**
     * @brief What the dock tab shows: the counts as badges, and whether to play the glow now.
     */
    struct Attention
    {
        AttentionCounts counts{};
        bool pulse{false};

        friend bool operator==(Attention const&, Attention const&) = default;
    };

    /**
     * @brief Decides when the dock tab glows and whether a click on it should jump to the newest conflict. Pure,
     *        tested natively; DockTabAttention feeds it and forwards its results to the tab.
     */
    class AttentionState
    {
      public:
        /**
         * @brief How long one glow plays. Conflicts arriving within this time of the last pulse do not pulse again.
         */
        static constexpr std::chrono::milliseconds pulseWindow{2500};

        /**
         * @brief Takes the current counts, seen at @p now (any steady clock). More conflicts than before make them
         *        unseen and pulse, unless the last pulse began less than @ref pulseWindow ago. Failures never pulse.
         *        No conflicts left clears unseen.
         * @return What the tab shows now, nothing when neither the counts changed nor a pulse is due.
         */
        std::optional<Attention>
        update([[maybe_unused]] AttentionCounts const& counts, [[maybe_unused]] std::chrono::milliseconds now)
        {
            return std::nullopt;
        }

        /**
         * @return Whether conflicts arrived since the last @ref acknowledge and some are still there.
         */
        bool unseen() const
        {
            return unseen_;
        }

        /**
         * @brief The user jumped to the newest conflict, what arrived so far counts as seen.
         */
        void acknowledge()
        {}

        /**
         * @return The counts of the last @ref update.
         */
        AttentionCounts const& counts() const
        {
            return counts_;
        }

        /**
         * @brief Forgets the counts, unseen and the last pulse, the session ended.
         */
        void reset()
        {}

      private:
        AttentionCounts counts_{};
        std::optional<std::chrono::milliseconds> lastPulse_{};
        bool unseen_{false};
    };
}
