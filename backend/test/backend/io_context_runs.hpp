#pragma once

#include <backend/file_tracking/directory_watch_service.hpp>
#include <persistence/state/sftp_options.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace Test
{
    /**
     * @brief Runs the handlers of @p ioc until @p condition holds, at most for @p timeout. Waits for the handlers that
     *        other threads post instead of spinning while there is none.
     */
    inline bool runUntil(
        boost::asio::io_context& ioc,
        std::function<bool()> const& condition,
        std::chrono::milliseconds timeout = std::chrono::seconds{5}
    )
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        ioc.restart();
        const auto keepRunning = boost::asio::make_work_guard(ioc);
        while (std::chrono::steady_clock::now() < deadline)
        {
            ioc.run_one_for(std::chrono::milliseconds{20});
            ioc.poll();
            if (condition())
                return true;
        }
        return false;
    }

    /**
     * @brief Runs the handlers of @p ioc until none is left, the timers of the file tracking are one-shot. A watch
     *        event still on its way is not waited for, see flushWatch.
     * @return false when handlers were still left after @p timeout.
     */
    inline bool runUntilIdle(boost::asio::io_context& ioc, std::chrono::milliseconds timeout = std::chrono::seconds{10})
    {
        ioc.restart();
        ioc.run_for(timeout);
        return ioc.stopped();
    }

    /**
     * @brief Waits until the watch of @p directory delivered everything that happened in it before, then until the
     *        handlers of @p ioc ran out. The marker is named like a download in progress, which the file tracking
     *        drops. It joins the watch that exists, with its recursion, so the marker comes after the earlier events.
     */
    inline void flushWatch(boost::asio::io_context& ioc, std::filesystem::path const& directory, bool recursive)
    {
        static std::atomic_int flushes{0};
        const auto seen = std::make_shared<std::atomic_bool>(false);
        const auto marker = fmt::format("flush{}{}", ++flushes, std::string{Persistence::defaultTempFileSuffix});
        const auto subscription = FileTracking::DirectoryWatchService::shared()->subscribe(
            directory,
            recursive,
            [seen, marker](FileTracking::DirectoryEvent const& event)
            {
                if (std::filesystem::path{event.filename}.filename() == marker)
                    *seen = true;
            }
        );
        ASSERT_TRUE(subscription.has_value()) << directory;
        std::ofstream{directory / marker};
        ASSERT_TRUE(runUntil(
            ioc,
            [&seen]()
            {
                return seen->load();
            },
            std::chrono::seconds{10}
        )) << directory;
        std::filesystem::remove(directory / marker);
        ASSERT_TRUE(runUntilIdle(ioc));
    }
}
