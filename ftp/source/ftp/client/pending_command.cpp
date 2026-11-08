#include <ftp/client/pending_command.hpp>
#include <ftp/protocol/reply_code.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <utility>

namespace FileTransferProtocol
{
    PendingCommand::PendingCommand(
        boost::asio::any_io_executor strand,
        std::optional<Command> command,
        std::string bytes
    )
        : command_{std::move(command)}
        , bytes_{std::move(bytes)}
        , signal_{std::move(strand), boost::asio::steady_timer::time_point::max()}
    {}

    boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> PendingCommand::nextReply()
    {
        bool cancelled = false;
        while (replies_.empty() && !failure_ && !finalTaken_ && !cancelled)
        {
            signal_.expires_at(boost::asio::steady_timer::time_point::max());
            co_await signal_.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            const auto state = co_await boost::asio::this_coro::cancellation_state;
            cancelled = state.cancelled() != boost::asio::cancellation_type::none;
        }

        if (!replies_.empty())
        {
            auto reply = std::move(replies_.front());
            replies_.pop_front();
            finalTaken_ = !isPreliminary(reply.code);
            co_return reply;
        }
        if (failure_)
            co_return std::unexpected(*failure_);
        if (cancelled)
        {
            co_return std::unexpected(
                RemoteIo::RemoteError{
                    .kind = RemoteIo::RemoteErrorKind::Cancelled,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .message = "The wait for a reply was cancelled",
                }
            );
        }
        co_return std::unexpected(
            RemoteIo::RemoteError{
                .kind = RemoteIo::RemoteErrorKind::Failure,
                .protocol = RemoteIo::Protocol::Ftp,
                .message = "The final reply was taken already",
            }
        );
    }

    boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> PendingCommand::finalReply()
    {
        auto reply = co_await nextReply();
        while (reply && isPreliminary(reply->code))
            reply = co_await nextReply();
        co_return reply;
    }

    std::optional<Verb> PendingCommand::verb() const
    {
        if (!command_)
            return std::nullopt;
        return command_->verb;
    }

    PendingCommand::Stage PendingCommand::stage() const
    {
        return stage_;
    }

    bool PendingCommand::answered() const
    {
        return stage_ == Stage::Answered || stage_ == Stage::Completed;
    }

    void PendingCommand::take(Reply reply)
    {
        stage_ = isPreliminary(reply.code) ? Stage::Answered : Stage::Completed;
        replies_.push_back(std::move(reply));
        wake();
    }

    void PendingCommand::fail(RemoteIo::RemoteError error)
    {
        stage_ = Stage::Completed;
        failure_ = std::move(error);
        wake();
    }

    void PendingCommand::wake()
    {
        signal_.cancel();
    }
}
