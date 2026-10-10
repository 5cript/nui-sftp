#pragma once

#ifndef _WIN32
#    include <backend/process/fork_pool.hpp>
#    include <persistence/state/termios.hpp>
#    include <roar/utility/base64.hpp>

#    include <boost/asio/executor_work_guard.hpp>
#    include <fmt/format.h>
#    include <boost/asio/io_context.hpp>
#    include <gtest/gtest.h>
#    include <nlohmann/json.hpp>

#    include <chrono>
#    include <condition_variable>
#    include <cstdlib>
#    include <filesystem>
#    include <functional>
#    include <mutex>
#    include <optional>
#    include <string>
#    include <thread>
#    include <vector>

extern std::filesystem::path programDirectory;

namespace Test
{
    class ForkPoolTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // The worker logs below %state_home2%, keep that inside the build directory.
            if (auto const* previous = std::getenv("XDG_STATE_HOME"))
                previousStateHome_ = previous;
            ::setenv("XDG_STATE_HOME", (programDirectory / "temp" / "fork_pool_state").c_str(), 1);

            ioThread_ = std::thread{[this]()
                {
                    context_.run();
                }};
            pool_.start(
                context_.get_executor(),
                [this](nlohmann::json const& message)
                {
                    {
                        std::scoped_lock lock{mutex_};
                        messages_.push_back(message);
                    }
                    changed_.notify_all();
                }
            );
        }

        void TearDown() override
        {
            pool_.stop();
            workGuard_.reset();
            context_.stop();
            ioThread_.join();
            if (previousStateHome_)
                ::setenv("XDG_STATE_HOME", previousStateHome_->c_str(), 1);
            else
                ::unsetenv("XDG_STATE_HOME");
        }

        void spawn(std::string const& id, std::string const& exe, std::vector<std::string> const& args)
        {
            pool_.send({
                {"id", id},
                {"command", "spawn"},
                {"payload",
                    {
                        {"exe", exe},
                        {"args", args},
                        {"env", nlohmann::json{{"PATH", "/usr/bin:/bin"}}},
                        {"termios", Persistence::Termios::saneDefaults()},
                        {"responseId", id + "-open"},
                    }},
            });
        }

        std::optional<nlohmann::json> waitForMessage(
            std::function<bool(nlohmann::json const&)> const& predicate,
            std::chrono::seconds timeout = std::chrono::seconds{10}
        )
        {
            std::unique_lock lock{mutex_};
            std::optional<nlohmann::json> found;
            changed_.wait_for(
                lock,
                timeout,
                [&]()
                {
                    for (auto const& message : messages_)
                    {
                        if (predicate(message))
                        {
                            found = message;
                            return true;
                        }
                    }
                    return false;
                }
            );
            return found;
        }

        std::optional<nlohmann::json> waitForType(std::string const& id, std::string const& type)
        {
            return waitForMessage(
                [&](nlohmann::json const& message)
                {
                    return message.value("id", "") == id && message.value("type", "") == type;
                }
            );
        }

        std::string collectedOutput(std::string const& id)
        {
            std::scoped_lock lock{mutex_};
            std::string output;
            for (auto const& message : messages_)
            {
                if (message.value("id", "") == id && message.value("type", "") == "stdout")
                    output += Roar::base64Decode(message["data"].get<std::string>());
            }
            return output;
        }

        bool waitForOutput(std::string const& id, std::string const& needle)
        {
            return waitForMessage(
                [&](nlohmann::json const&)
                {
                    std::string output;
                    for (auto const& message : messages_)
                    {
                        if (message.value("id", "") == id && message.value("type", "") == "stdout")
                            output += Roar::base64Decode(message["data"].get<std::string>());
                    }
                    return output.find(needle) != std::string::npos;
                }
            ).has_value();
        }

        boost::asio::io_context context_{};
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard_{
            boost::asio::make_work_guard(context_)
        };
        std::thread ioThread_{};
        ForkPool pool_{};
        std::mutex mutex_{};
        std::condition_variable changed_{};
        std::vector<nlohmann::json> messages_{};
        std::optional<std::string> previousStateHome_{};
    };

    TEST_F(ForkPoolTests, SpawnedProcessReportsOutputAndExitCode)
    {
        spawn("exit", "/bin/sh", {"-c", "printf ready; exit 3"});

        ASSERT_TRUE(waitForType("exit", "open").has_value());
        const auto exit = waitForType("exit", "exit");
        ASSERT_TRUE(exit.has_value());
        EXPECT_EQ((*exit)["code"].get<int>(), 3);
        EXPECT_NE(collectedOutput("exit").find("ready"), std::string::npos);
    }

    TEST_F(ForkPoolTests, StdinReachesTheProcessThroughThePty)
    {
        spawn("cat", "/bin/cat", {});
        ASSERT_TRUE(waitForType("cat", "open").has_value());

        pool_.send({
            {"id", "cat"},
            {"command", "stdin"},
            {"payload", {{"data", Roar::base64Encode(std::string{"marker\n"})}}},
        });
        EXPECT_TRUE(waitForOutput("cat", "marker"));

        pool_.send({{"id", "cat"}, {"command", "kill"}});
        EXPECT_TRUE(waitForType("cat", "exit").has_value());
    }

    TEST_F(ForkPoolTests, StdinLargerThanThePtyBufferArrivesCompletely)
    {
        spawn("paste", "/bin/cat", {});
        ASSERT_TRUE(waitForType("paste", "open").has_value());

        // A macOS pty takes about 1 KiB at a time, the rest has to wait instead of being dropped.
        std::string paste;
        for (int line = 0; line < 200; ++line)
            paste += fmt::format("line-{:03}-{}\n", line, std::string(90, 'x'));
        pool_.send({
            {"id", "paste"},
            {"command", "stdin"},
            {"payload", {{"data", Roar::base64Encode(paste)}}},
        });
        EXPECT_TRUE(waitForOutput("paste", "line-199-"));

        pool_.send({{"id", "paste"}, {"command", "kill"}});
        EXPECT_TRUE(waitForType("paste", "exit").has_value());
    }

    TEST_F(ForkPoolTests, ListProcessesFindsTheProcessOnItsTerminal)
    {
        spawn("sleeper", "/bin/sh", {"-c", "printf started; exec sleep 30"});
        ASSERT_TRUE(waitForType("sleeper", "open").has_value());
        ASSERT_TRUE(waitForOutput("sleeper", "started"));

        // "started" arrives before the exec, so ask until the terminal shows sleep.
        std::optional<nlohmann::json> list;
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            const auto responseId = fmt::format("sleeper-list-{}", attempt);
            pool_.send({
                {"id", "sleeper"},
                {"command", "listProcesses"},
                {"payload", {{"responseId", responseId}}},
            });
            list = waitForMessage(
                [&](nlohmann::json const& message)
                {
                    return message.value("responseId", "") == responseId;
                }
            );
            ASSERT_TRUE(list.has_value());
            ASSERT_TRUE(list->contains("procs")) << list->dump();
            if (!(*list)["procs"].empty() &&
                (*list)["procs"].front()["cmdline"].get<std::string>().find("sleep") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        ASSERT_FALSE((*list)["procs"].empty()) << list->dump();
        EXPECT_NE((*list)["procs"].front()["cmdline"].get<std::string>().find("sleep"), std::string::npos)
            << list->dump();

        pool_.send({{"id", "sleeper"}, {"command", "kill"}});
        EXPECT_TRUE(waitForType("sleeper", "exit").has_value());
    }

    TEST_F(ForkPoolTests, ResizeDoesNotDisturbTheProcess)
    {
        spawn("size", "/bin/sh", {"-c", "read line; stty size"});
        ASSERT_TRUE(waitForType("size", "open").has_value());

        pool_.send({{"id", "size"}, {"command", "resize"}, {"payload", {{"cols", 123}, {"rows", 45}}}});
        pool_.send({
            {"id", "size"},
            {"command", "stdin"},
            {"payload", {{"data", Roar::base64Encode(std::string{"go\n"})}}},
        });
        EXPECT_TRUE(waitForOutput("size", "45 123"));
        EXPECT_TRUE(waitForType("size", "exit").has_value());
    }
}
#endif
