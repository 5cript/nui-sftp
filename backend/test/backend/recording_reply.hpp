#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Test
{
    /**
     * @brief What a RecordingReply was answered with: the JSON of a reply or the message of an error.
     */
    struct RecordedAnswer
    {
        std::optional<nlohmann::json> json{};
        std::optional<std::string> error{};
    };

    /**
     * @brief Answers like an RpcOnce and records every answer, from any thread. Its copies share the record.
     */
    class RecordingReply
    {
      public:
        void operator()(nlohmann::json&& json) const
        {
            record(RecordedAnswer{.json = std::move(json)});
        }

        void error(std::string const& message) const
        {
            record(RecordedAnswer{.error = message});
        }

        /**
         * @brief The first answer, once there is one.
         */
        std::shared_future<RecordedAnswer> answered() const
        {
            return state_->first;
        }

        std::size_t answers() const
        {
            std::scoped_lock lock{state_->mutex};
            return state_->answers.size();
        }

      private:
        struct State
        {
            std::mutex mutex{};
            std::promise<RecordedAnswer> firstAnswer{};
            std::shared_future<RecordedAnswer> first{firstAnswer.get_future().share()};
            std::vector<RecordedAnswer> answers{};
        };

        void record(RecordedAnswer answer) const
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->answers.empty())
                state_->firstAnswer.set_value(answer);
            state_->answers.push_back(std::move(answer));
        }

        std::shared_ptr<State> state_{std::make_shared<State>()};
    };
}
