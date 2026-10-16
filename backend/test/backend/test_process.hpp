#pragma once

#include <backend/process/environment.hpp>
#include <backend/process/process.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Test
{
    class ProcessTests : public ::testing::Test
    {
      protected:
        void TearDown() override
        {
            work_.reset();
            context_.stop();
            runner_.join();
        }

        /**
         * @brief Waits until @p done holds for what was read, at most 10 seconds.
         */
        template <typename PredicateT>
        bool waitFor(PredicateT done)
        {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock,
                std::chrono::seconds{10},
                [&]()
                {
                    return done();
                }
            );
        }

        bool append(std::string& target, std::string_view data)
        {
            {
                std::scoped_lock lock{mutex_};
                target.append(data);
            }
            changed_.notify_all();
            return true;
        }

        boost::asio::io_context context_{};
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_{context_.get_executor()};
        std::thread runner_{[this]()
                            {
                                context_.run();
                            }};
        std::mutex mutex_{};
        std::condition_variable changed_{};
        std::string output_{};
        std::string errors_{};
    };

#ifndef _WIN32
    TEST_F(ProcessTests, ChildWithoutLauncherTalksThroughThePipes)
    {
        auto process = std::make_shared<Process>(context_.get_executor(), []() {});
        Environment environment{};
        environment.loadFromCurrent();
        process->spawn("sh", {"-c", "read line; echo \"got $line\"; echo oops >&2"}, environment);
        process->startReading(
            [this](std::string_view data)
            {
                return append(output_, data);
            },
            [this](std::string_view data)
            {
                return append(errors_, data);
            }
        );
        process->write("hello\n");

        EXPECT_TRUE(waitFor(
            [this]()
            {
                return output_.find("got hello\n") != std::string::npos && errors_.find("oops\n") != std::string::npos;
            }
        )) << "stdout: '" << output_ << "', stderr: '" << errors_ << "'";
        EXPECT_EQ(process->exitSync(std::chrono::seconds{5}), 0);
    }
#endif
}
