#include <ftp/pool/lease.hpp>
#include <ftp/pool/pool.hpp>
#include <ftp/client/without_throwing.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>

#include <cassert>
#include <utility>

namespace FileTransferProtocol
{
    namespace asio = boost::asio;

    Lease::Lease(std::weak_ptr<Pool> pool, std::shared_ptr<PooledConnection> connection)
        : pool_{std::move(pool)}
        , connection_{std::move(connection)}
    {}

    Lease::~Lease()
    {
        release();
    }

    Lease::Lease(Lease&& other) noexcept
        : pool_{std::move(other.pool_)}
        , connection_{std::move(other.connection_)}
    {}

    Lease& Lease::operator=(Lease&& other) noexcept
    {
        if (this != &other)
        {
            release();
            pool_ = std::move(other.pool_);
            connection_ = std::move(other.connection_);
        }
        return *this;
    }

    std::shared_ptr<ControlConnection> const& Lease::control() const
    {
        assert(connection_ && "the lease was released");
        return connection_->control;
    }

    LoginResult const& Lease::login() const
    {
        assert(connection_ && "the lease was released");
        return connection_->login;
    }

    TransferSession const& Lease::transferSession() const
    {
        assert(connection_ && "the lease was released");
        return connection_->transferSession;
    }

    std::string const& Lease::name() const
    {
        assert(connection_ && "the lease was released");
        return connection_->name;
    }

    void Lease::release()
    {
        auto connection = std::exchange(connection_, nullptr);
        if (!connection)
            return;
        const auto control = connection->control;
        const auto strand = control->strand();
        // Only the connection's strand knows whether it is still open, once its commands ended: the next holder would
        // wait for the drain of an abort and fail with it. The pool's strand takes it back.
        asio::co_spawn(
            strand,
            Detail::withoutThrowing([](std::weak_ptr<Pool> pool, std::shared_ptr<PooledConnection> connection)
                                        -> asio::awaitable<void>
            {
                co_await connection->control->untilIdle();
                const auto open = connection->control->state() == ControlState::Open;
                const auto locked = pool.lock();
                if (!locked)
                {
                    connection->control->close();
                    co_return;
                }
                asio::post(
                    locked->strand(),
                    [locked, connection, open]()
                    {
                        locked->release(connection, open);
                    }
                );
            }(std::exchange(pool_, {}), std::move(connection))),
            // The pool learns of the end of the connection and drops it, a lease that never came back held its slot.
            Detail::endingOnException(
                "The release of a connection",
                [control]()
                {
                    control->close();
                }
            )
        );
    }
}
