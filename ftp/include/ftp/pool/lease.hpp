#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/login.hpp>
#include <ftp/client/transfer.hpp>
#include <ftp/pool/pooled_connection.hpp>

#include <memory>
#include <string>

namespace FileTransferProtocol
{
    class Pool;

    /**
     * @brief A connection of a Pool, its holder's alone until release(). Commands and transfers on it run on the strand
     *        of its control connection. Commands that belong together, RNFR and RNTO or CWD and LIST, go out within one
     *        lease.
     *
     * A lease may end on any thread. The pool takes the connection back once its commands ended, the drain of an ABOR
     * included, and its strand said whether it is still open; one that ended is dropped.
     */
    class Lease
    {
      public:
        /**
         * @brief Holds no connection, as after release().
         */
        Lease() = default;
        Lease(std::weak_ptr<Pool> pool, std::shared_ptr<PooledConnection> connection);
        /**
         * @brief release()
         */
        ~Lease();
        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        /**
         * @brief Used on its own strand only. Not after release().
         */
        std::shared_ptr<ControlConnection> const& control() const;

        /**
         * @brief What the login of the connection learned. Not after release().
         */
        LoginResult const& login() const;

        /**
         * @brief What a Download or an Upload on this connection takes. Not after release().
         */
        TransferSession const& transferSession() const;

        /**
         * @brief C0 for the metadata connection, C1 and up for transfer connections. Not after release().
         */
        std::string const& name() const;

        /**
         * @brief Gives the connection back, from any thread. Does nothing after a release or a move.
         */
        void release();

      private:
        std::weak_ptr<Pool> pool_{};
        std::shared_ptr<PooledConnection> connection_{};
    };
}
