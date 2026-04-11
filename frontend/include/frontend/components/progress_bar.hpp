#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <string>

namespace Components
{
    class ProgressBar
    {
      public:
        struct Settings
        {
            std::string height{"30px"};
            long long min{0};
            long long max{100};
            bool showMinMax{false};
            bool byteMode{false};
        };
        ProgressBar(Settings settings);

        ROAR_PIMPL_SPECIAL_FUNCTIONS(ProgressBar);

        Nui::ElementRenderer operator()(std::string const& extraStyleOptions = "") const;

        /**
         * Set the progress of the progress bar.
         * Cannot be set if the progress bar is not mounted.
         */
        void setProgress(long long current);

        /**
         * @brief Set the maximum value of the progress bar.
         *
         * @param max The maximum value.
         */
        void max(long long max);

        /**
         * @brief Get the maximum value of the progress bar.
         *
         * @return long long
         */
        long long max() const;

        /**
         * @brief Sometimes a progress bar from 0 to 0 makes sense. Initially shown as 0% but once marked as complete,
         * it should show 100%. Call this to enable that behavior.
         */
        void setZeroAsComplete();

      private:
        void updateText();
        void recalculate();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}