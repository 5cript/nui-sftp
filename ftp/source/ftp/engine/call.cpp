#include <ftp/client/control_connection.hpp>
#include <ftp/engine/call.hpp>

#include <boost/asio/cancellation_type.hpp>

#include <utility>

namespace FileTransferProtocol
{
    void CallScope::touch(std::shared_ptr<ControlConnection> const& connection)
    {
        touched_.push_back(connection);
    }

    void CallScope::closeTouched()
    {
        for (auto const& touched : touched_)
        {
            if (const auto connection = touched.lock())
                connection->close();
        }
    }

    void CallScope::allowMore(std::chrono::milliseconds more)
    {
        if (allowMore_)
            allowMore_(more);
    }

    void CallScope::onAllowMore(AllowMore allowMore)
    {
        allowMore_ = std::move(allowMore);
    }

    namespace Detail
    {
        CallState::CallState(boost::asio::any_io_executor const& sessionStrand, SyncLimits limits)
            : limits_{limits}
            , timer_{sessionStrand}
        {}

        SyncLimits const& CallState::limits() const
        {
            return limits_;
        }

        void CallState::extendBackstop(std::chrono::milliseconds more)
        {
            limits_.backstop += more;
        }

        std::shared_ptr<CallScope> const& CallState::scope() const
        {
            return scope_;
        }

        boost::asio::cancellation_slot CallState::slot()
        {
            return signal_.slot();
        }

        boost::asio::steady_timer& CallState::timer()
        {
            return timer_;
        }

        void CallState::cancel()
        {
            draining_ = true;
            signal_.emit(boost::asio::cancellation_type::terminal);
        }

        bool CallState::draining() const
        {
            return draining_;
        }
    }
}
