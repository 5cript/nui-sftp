#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/metadata.hpp>
#include <ftp/client/transfer.hpp>
#include <ftp/engine/call.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/engine/ring_options.hpp>
#include <ftp/pool/lease.hpp>
#include <ftp/protocol/listing.hpp>
#include <ftp/remote/file_system_operations.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system_operations.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace FileTransferProtocol
{
    struct FtpFileSystemOptions
    {
        /** @brief The bounds of each operation of the file system, see Engine::run(). */
        SyncLimits operationLimits{};
        /**
         * @brief The bounds of each synchronous read, write and finish of a stream. Beyond the transfer's own
         *        inactivity and reply deadlines, which end a stalled transfer first.
         */
        SyncLimits transferCallLimits{.backstop = std::chrono::seconds{45}, .drain = std::chrono::seconds{1}};
        TransferOptions transfer{};
        ListingLimits listing{};
        /** @brief The most bytes one read or write of a stream moves. */
        std::size_t lengthLimit{256u * 1024u};
        /** @brief The ring of each asynchronous read or write. */
        RingOptions ring{};
        /**
         * @brief What each link that a listing asks the server about adds to the backstop of that listing. Its
         *        commands have their own timeout, this only keeps the backstop behind them.
         */
        std::chrono::milliseconds linkLookupAllowance{std::chrono::seconds{1}};
    };

    /**
     * @brief What the file systems of one session share, fixed once its first login told it.
     */
    struct FileSystemSession
    {
        std::shared_ptr<Engine> engine{};
        /** @brief What the session learned about the commands of its server. */
        std::shared_ptr<ServerDialect> dialect = std::make_shared<ServerDialect>();
        /** @brief The directory the first login started in, from PWD, empty when the server did not tell. */
        std::string homeDirectory{};
        /** @brief How many transfers run at once, the transfer connections of the pool. */
        std::size_t parallelTransfers{1u};
    };

    /**
     * @brief An operation that runs with the commands of a connection of the pool.
     */
    template <typename OperationT>
    concept RunsOnACommandConnection = requires(MetadataSession session, OperationT operation) {
        runOperation(std::move(session), std::move(operation));
    };

    namespace Detail
    {
        /**
         * @brief @p work with throw_if_cancelled(false): a cancel that no operation under way took must not throw at
         *        the next step.
         */
        template <typename ValueT>
        boost::asio::awaitable<std::expected<ValueT, RemoteIo::RemoteError>>
        withoutThrowing(boost::asio::awaitable<std::expected<ValueT, RemoteIo::RemoteError>> work)
        {
            co_await boost::asio::this_coro::throw_if_cancelled(false);
            co_return co_await std::move(work);
        }

        /**
         * @brief Runs @p work on the strand of @p control and touches it in @p scope, from the session strand. An
         *        exception fails the session, see Engine::run().
         */
        template <typename ValueT>
        boost::asio::awaitable<std::expected<ValueT, RemoteIo::RemoteError>> onConnection(
            std::shared_ptr<ControlConnection> control,
            std::shared_ptr<CallScope> scope,
            boost::asio::awaitable<std::expected<ValueT, RemoteIo::RemoteError>> work
        )
        {
            scope->touch(control);
            auto [exception, result] = co_await boost::asio::co_spawn(
                control->strand(),
                withoutThrowing<ValueT>(std::move(work)),
                boost::asio::as_tuple(boost::asio::use_awaitable)
            );
            if (exception)
                std::rethrow_exception(exception);
            co_return std::move(result);
        }

        RemoteIo::RemoteError sessionEndedError();

        /**
         * @brief What a metadata operation of @p session runs on with the connection of @p lease.
         */
        MetadataSession
        metadataSessionOf(Lease const& lease, FileSystemSession const& session, FtpFileSystemOptions const& options);

        /**
         * @brief Takes a connection for commands of the pool and runs @p operation there, on the session strand.
         *
         * @param held A connection the caller holds itself, see Pool::acquireCommand().
         */
        template <RunsOnACommandConnection OperationT>
        boost::asio::awaitable<RemoteIo::OperationResult<OperationT>> onCommandConnection(
            FileSystemSession session,
            FtpFileSystemOptions options,
            std::shared_ptr<CallScope> scope,
            OperationT operation,
            std::shared_ptr<ControlConnection> held = nullptr
        )
        {
            const auto pool = session.engine->pool();
            if (!pool)
                co_return std::unexpected(sessionEndedError());
            auto lease = co_await pool->acquireCommand(std::move(held));
            if (!lease)
                co_return std::unexpected(std::move(lease).error());
            co_return co_await onConnection<typename OperationT::Result>(
                lease->control(),
                std::move(scope),
                runOperation(metadataSessionOf(*lease, session, options), std::move(operation))
            );
        }

        /**
         * @brief onCommandConnection() for a listing, which learns from the listing how many links it has to ask the
         *        server about: FtpFileSystemOptions::linkLookupAllowance for each moves its backstop.
         */
        boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::ListDirectory>> onCommandConnection(
            FileSystemSession session,
            FtpFileSystemOptions options,
            std::shared_ptr<CallScope> scope,
            RemoteIo::ListDirectory operation,
            std::shared_ptr<ControlConnection> held = nullptr
        );
    }
}
