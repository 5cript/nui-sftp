#pragma once

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Test
{
    /**
     * @brief Records the messages logged as errors while it lives. The tests log nothing otherwise.
     */
    class ErrorLogCapture
    {
      public:
        ErrorLogCapture()
            : sink_{std::make_shared<spdlog::sinks::callback_sink_mt>(
                  [this](spdlog::details::log_msg const& message)
                  {
                      std::scoped_lock lock{mutex_};
                      messages_.emplace_back(message.payload.data(), message.payload.size());
                  }
              )}
            , previousLevel_{spdlog::get_level()}
        {
            sink_->set_level(spdlog::level::err);
            // Added while nothing is logged, so no other thread walks the sinks meanwhile.
            spdlog::default_logger()->sinks().push_back(sink_);
            spdlog::set_level(spdlog::level::err);
        }
        ~ErrorLogCapture()
        {
            spdlog::set_level(previousLevel_);
            std::erase(spdlog::default_logger()->sinks(), sink_);
        }
        ErrorLogCapture(ErrorLogCapture const&) = delete;
        ErrorLogCapture& operator=(ErrorLogCapture const&) = delete;
        ErrorLogCapture(ErrorLogCapture&&) = delete;
        ErrorLogCapture& operator=(ErrorLogCapture&&) = delete;

        /**
         * @brief How many errors mention @p text.
         */
        std::size_t mentioning(std::string_view text) const
        {
            std::scoped_lock lock{mutex_};
            return static_cast<std::size_t>(std::ranges::count_if(
                messages_,
                [text](std::string const& message)
                {
                    return message.contains(text);
                }
            ));
        }

      private:
        mutable std::mutex mutex_{};
        std::vector<std::string> messages_{};
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink_;
        spdlog::level::level_enum previousLevel_;
    };
}
