#pragma once

#include <backend/file_tracking/reported_change.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace FileTracking
{
    /**
     * @brief How long the content of a file has to stay unchanged before its change is reported. One save shows as
     *        several events (inotify reports the writes and the close), they become one change.
     */
    inline constexpr std::chrono::milliseconds changeSettleTime{300};

    /**
     * @brief Holds back the content changes of files until they settled, so a save is reported once: no event and no
     *        other size or modification time for the settle time. Windows reports the writes to a file that stays
     *        open only when someone else looks at it, which an upload does. Only content changes of the same file
     *        merge. Every other change goes at once, after the held back changes it concerns: those at its path or
     *        below it, and at or below where it moved from. Lives on its strand: every member must be called from it.
     */
    class SettledChanges : public std::enable_shared_from_this<SettledChanges>
    {
      public:
        /**
         * @param send Reports a change, called on the strand.
         */
        SettledChanges(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            ChangeSink send,
            std::chrono::milliseconds settleTime = changeSettleTime
        );
        ~SettledChanges() = default;
        SettledChanges(SettledChanges const&) = delete;
        SettledChanges& operator=(SettledChanges const&) = delete;
        SettledChanges(SettledChanges&&) = delete;
        SettledChanges& operator=(SettledChanges&&) = delete;

        /**
         * @brief Reports @p change.
         * @param isFileContent Whether it changed the content of a file: it is held back until nothing changed
         *        there for the settle time, a later content change of the same file replaces it. An addition stays
         *        an addition.
         */
        void add(ReportedChange change, bool isFileContent);

        /**
         * @brief What a held back file looks like from outside.
         */
        struct Appearance
        {
            std::uintmax_t size;
            std::filesystem::file_time_type modified;

            friend bool operator==(Appearance const&, Appearance const&) = default;
        };

      private:
        struct Pending
        {
            ReportedChange change;
            /** @brief Orders the held back changes by when they last changed. */
            std::uint64_t sequence;
            /** @brief Tells the wait that ends from the ones restarting the timer replaced. */
            std::uint64_t generation;
            boost::asio::steady_timer timer;
            /** @brief At the last event or the last end of the settle time, nothing when the file was not there. */
            std::optional<Appearance> appearance{};
        };

        /**
         * @brief Sends the held back changes below @p base, and at it when @p includingBase, in their order.
         */
        void flush(std::filesystem::path const& base, bool includingBase);
        void hold(ReportedChange change);
        /**
         * @brief Sends the change held back at @p key once the settle time passed without a change of the file.
         */
        void settle(std::filesystem::path const& key, Pending& pending);

      private:
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        ChangeSink send_;
        std::chrono::milliseconds settleTime_;
        std::map<std::filesystem::path, std::unique_ptr<Pending>> pending_;
        std::uint64_t nextSequence_;
    };
}
