#include <backend/file_tracking/settled_changes.hpp>
#include <backend/file_tracking/change_filter.hpp>

#include <boost/asio/error.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace FileTracking
{
    SettledChanges::SettledChanges(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        ChangeSink send,
        std::chrono::milliseconds settleTime
    )
        : strand_{std::move(strand)}
        , send_{std::move(send)}
        , settleTime_{settleTime}
        , pending_{}
        , nextSequence_{0}
    {}

    void SettledChanges::add(ReportedChange change, bool isFileContent)
    {
        flush(change.path, /*includingBase=*/!isFileContent);
        flush(change.oldPath, /*includingBase=*/true);
        if (isFileContent)
            return hold(std::move(change));
        send_(change);
    }

    void SettledChanges::hold(ReportedChange change)
    {
        // Normal like the bases flush() looks below.
        const auto key = change.path.lexically_normal();
        auto found = pending_.find(key);
        if (found == pending_.end())
            found = pending_
                        .emplace(
                            key,
                            std::make_unique<Pending>(Pending{
                                .change = change,
                                .sequence = 0,
                                .generation = 0,
                                .timer = boost::asio::steady_timer{strand_},
                            })
                        )
                        .first;
        else if (found->second->change.action == FileAction::Added)
            change.action = FileAction::Added;
        auto& pending = *found->second;
        pending.change = std::move(change);
        pending.sequence = nextSequence_++;
        const auto generation = ++pending.generation;
        pending.timer.expires_after(settleTime_);
        pending.timer.async_wait(
            [weak = weak_from_this(), key, generation](boost::system::error_code const& error)
            {
                if (error == boost::asio::error::operation_aborted)
                    return;
                const auto self = weak.lock();
                if (!self)
                    return;
                const auto settled = self->pending_.find(key);
                // A wait that completed before a later change restarted the timer is not the last one.
                if (settled == self->pending_.end() || settled->second->generation != generation)
                    return;
                const auto settledChange = std::move(settled->second->change);
                self->pending_.erase(settled);
                self->send_(settledChange);
            }
        );
    }

    void SettledChanges::flush(std::filesystem::path const& base, bool includingBase)
    {
        if (base.empty())
            return;
        // Paths order by their elements, so what lies at and below the base follows it in one run.
        const auto normalBase = base.lexically_normal();
        std::vector<std::unique_ptr<Pending>> flushed;
        for (auto pending = pending_.lower_bound(normalBase);
             pending != pending_.end() && isAtOrBelow(normalBase, pending->first);)
        {
            if (!includingBase && pending->first == normalBase)
            {
                ++pending;
                continue;
            }
            flushed.push_back(std::move(pending->second));
            pending = pending_.erase(pending);
        }
        std::ranges::sort(
            flushed,
            [](auto const& first, auto const& second)
            {
                return first->sequence < second->sequence;
            }
        );
        for (auto const& pending : flushed)
            send_(pending->change);
    }
}
