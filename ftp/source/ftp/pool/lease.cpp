#include <ftp/pool/lease.hpp>
#include <ftp/pool/pool.hpp>

#include <boost/asio/dispatch.hpp>
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
        const auto strand = connection->control->strand();
        // Only the connection's strand knows whether it is still open, the pool's strand takes it back.
        asio::dispatch(
            strand,
            [pool = std::exchange(pool_, {}), connection = std::move(connection)]()
            {
                const auto open = connection->control->state() == ControlState::Open;
                const auto locked = pool.lock();
                if (!locked)
                {
                    connection->control->close();
                    return;
                }
                asio::post(
                    locked->strand(),
                    [locked, connection, open]()
                    {
                        locked->release(connection, open);
                    }
                );
            }
        );
    }
}
