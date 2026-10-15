#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace FileExplorer
{
    /**
     * @brief The user's answer to "the destination exists, overwrite it?".
     */
    enum class OverwriteAnswer
    {
        /** @brief Overwrite this destination. */
        Yes,
        /** @brief Skip this candidate. */
        No,
        /** @brief Overwrite this and every later existing destination. */
        All,
        /** @brief Skip this and every later candidate whose destination exists. */
        None,
        /** @brief Stop asking, only what was accepted so far is transferred. */
        Cancel,
    };

    /**
     * @brief @p entry with the user's consent to replace its existing destination.
     */
    template <typename EntryT>
    EntryT approvedForOverwrite(EntryT entry)
    {
        entry.mayOverwrite = true;
        return entry;
    }

    /**
     * @brief Decides which transfer candidates go ahead, asking about each one whose destination exists.
     *
     * A candidate whose destination did not exist when probed is accepted without consent to overwrite, so one that
     * appears before the transfer runs fails instead of being replaced. Only candidates the user approved (Yes, All)
     * may replace their destination.
     */
    template <typename EntryT>
    class OverwriteDecisions
    {
      public:
        /**
         * @param candidates The entries to transfer, each with a destination and a mayOverwrite flag.
         * @param exists Index aligned with @p candidates: whether the destination exists. A missing answer counts as
         *        existing, so the user is asked.
         */
        OverwriteDecisions(std::vector<EntryT> candidates, std::vector<bool> exists)
            : candidates_{std::move(candidates)}
            , exists_{std::move(exists)}
        {}

        /**
         * @brief Accepts or skips every candidate up to the next one that needs an answer.
         *
         * @return That candidate, nullptr when every candidate is decided.
         */
        EntryT const* nextQuestion()
        {
            while (next_ < candidates_.size())
            {
                if (!destinationExists(next_))
                    accepted_.push_back(candidates_[next_]);
                else if (overwriteAll_)
                    accepted_.push_back(approvedForOverwrite(candidates_[next_]));
                else if (!skipAll_)
                    return &candidates_[next_];
                ++next_;
            }
            return nullptr;
        }

        /**
         * @brief Applies @p answer to the candidate @ref nextQuestion returned.
         */
        void answer(OverwriteAnswer answer)
        {
            if (next_ >= candidates_.size())
                return;
            switch (answer)
            {
                case OverwriteAnswer::All:
                    overwriteAll_ = true;
                    [[fallthrough]];
                case OverwriteAnswer::Yes:
                    accepted_.push_back(approvedForOverwrite(candidates_[next_]));
                    ++next_;
                    return;
                case OverwriteAnswer::None:
                    skipAll_ = true;
                    [[fallthrough]];
                case OverwriteAnswer::No:
                    ++next_;
                    return;
                case OverwriteAnswer::Cancel:
                    next_ = candidates_.size();
                    return;
            }
        }

        /**
         * @brief The accepted candidates in their order, taken out of this object.
         */
        std::vector<EntryT> takeAccepted()
        {
            return std::exchange(accepted_, {});
        }

      private:
        bool destinationExists(std::size_t index) const
        {
            return index >= exists_.size() || exists_[index];
        }

      private:
        std::vector<EntryT> candidates_;
        std::vector<bool> exists_;
        std::vector<EntryT> accepted_{};
        std::size_t next_{0};
        bool overwriteAll_{false};
        bool skipAll_{false};
    };
}
