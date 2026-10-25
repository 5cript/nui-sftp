#include <ssh/channel.hpp>
#include <ssh/session.hpp>

namespace SecureShell
{
    Channel::Channel(Session* owner, std::unique_ptr<ProcessingStrand> strand, std::unique_ptr<ssh::Channel> channel)
        : owner_{owner}
        , strand_{std::move(strand)}
        , channel_{std::move(channel)}
    {}
    Channel::~Channel()
    {
        /* close makes no sense, since this lives in a shared_ptr and will only ever end here when it was already
         * removed */
    }
    bool Channel::close(bool isBackElement)
    {
        if (strand_->isFinalized())
            return false;

        // Dont call on exit, its expected now and onExit is used for connection loss.
        closing_ = true;

        return strand_
            ->pushFinalPromiseTask(
                [this, isBackElement]()
                {
                    removeCallbacks();
                    if (channel_ && channel_->isOpen())
                    {
                        channel_->sendEof();
                        channel_->close();
                    }
                    channel_.reset();
                    owner_->channelRemoveItself(this, isBackElement);
                    return true;
                }
            )
            .get();
    }
    bool Channel::write(std::string data)
    {
        return strand_->pushTask(
            [this, data = std::move(data)]()
            {
                if (channel_)
                    channel_->write(data.data(), data.size());
            }
        );
    }
    std::future<int> Channel::resizePty(int cols, int rows)
    {
        auto promise = std::make_shared<std::promise<int>>();
        if (!strand_->pushTask(
                [this, cols, rows, promise]()
                {
                    if (channel_)
                        promise->set_value(channel_->changePtySize(cols, rows));
                    else
                        promise->set_value(SSH_ERROR);
                }
            ))
        {
            promise->set_value(SSH_ERROR);
        }
        return promise->get_future();
    }
    int Channel::onData(ssh_session, ssh_channel, void* data, uint32_t length, int isStderr, void* self)
    {
        auto* channel = static_cast<Channel*>(self);
        if (channel->closing_)
            return static_cast<int>(length);
        std::string const text{static_cast<char const*>(data), length};
        if (isStderr != 0)
            channel->onStderr_(text);
        else
            channel->onStdout_(text);
        return static_cast<int>(length);
    }
    void Channel::onEnd(ssh_session, ssh_channel, void* self)
    {
        // Seen while another task read the socket: the read task reports it in the next cycle.
        static_cast<Channel*>(self)->strand_->wake();
    }
    void Channel::registerCallbacks()
    {
        callbacks_ = {};
        callbacks_.userdata = this;
        callbacks_.channel_data_function = &Channel::onData;
        callbacks_.channel_eof_function = &Channel::onEnd;
        callbacks_.channel_close_function = &Channel::onEnd;
        ssh_callbacks_init(&callbacks_);
        callbacksRegistered_ = ssh_set_channel_callbacks(channel_->getCChannel(), &callbacks_) == SSH_OK;
    }
    void Channel::removeCallbacks()
    {
        if (callbacksRegistered_ && channel_)
            ssh_remove_channel_callbacks(channel_->getCChannel(), &callbacks_);
        callbacksRegistered_ = false;
    }
    ProcessingThread::PermanentTaskStatus Channel::readTask()
    {
        using enum ProcessingThread::PermanentTaskStatus;
        if (!onStdout_ || !onStderr_ || !onExit_ || !channel_)
            return Done;

        if (!callbacksRegistered_)
            registerCallbacks();

        constexpr static int bufferSize = 1024;
        char buffer[bufferSize];

        // Reads the socket without waiting. Output then reaches the callbacks; the buffer only holds what arrived
        // before they were registered.
        auto readOne = [this, &buffer](bool stdout_)
        {
            auto rdy = ssh_channel_poll_timeout(channel_->getCChannel(), 0, stdout_ ? 0 : 1);
            if (rdy < 0)
                return -1;

            while (rdy > 0)
            {
                const auto toRead = std::min(bufferSize, rdy);
                const auto bytesRead = ssh_channel_read(channel_->getCChannel(), buffer, toRead, stdout_ ? 0 : 1);
                if (bytesRead <= 0)
                    return -1;

                std::string data{buffer, static_cast<std::size_t>(bytesRead)};
                if (stdout_)
                    onStdout_(data);
                else
                    onStderr_(data);

                rdy -= bytesRead;
            }
            return rdy;
        };

        if (readOne(true) < 0 || readOne(false) < 0)
        {
            removeCallbacks();
            if (!closing_)
                onExit_();
            return Done;
        }
        // Without callbacks output read by other calls waits in the buffer, which only polling finds.
        return callbacksRegistered_ ? AwaitInput : Poll;
    }
    void Channel::startReading(
        std::function<void(std::string const&)> onStdout,
        std::function<void(std::string const&)> onStderr,
        std::function<void()> onExit
    )
    {
        onStdout_ = std::move(onStdout);
        onStderr_ = std::move(onStderr);
        onExit_ = std::move(onExit);

        auto [success, id] = strand_->pushPermanentTask(
            std::function<ProcessingThread::PermanentTaskStatus()>{
                [this]()
                {
                    return readTask();
                }
            }
        );
        if (!success)
        {
            // TODO: Do I want to throw here?
            throw std::runtime_error("Failed to start reading.");
        }
    }
}
