#pragma once

#include <ftp/engine/resolver.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <memory>

/**
 * @brief The engine threads and the resolver that every FTP session of the application shares. The threads run one
 *        io_context, an exception escaping a handler is logged and its thread goes on.
 *
 * Outlives every FTP session: destroy it after the SessionManager closed them all, never on one of its threads.
 */
class FtpEngineContext
{
  public:
    /**
     * @param threads The number of engine threads, at least 1.
     */
    explicit FtpEngineContext(std::size_t threads);
    /**
     * @brief Ends the lookups still waiting, then stops the engine threads and joins them. Handlers left over are
     *        destroyed on the calling thread.
     */
    ~FtpEngineContext();
    FtpEngineContext(FtpEngineContext const&) = delete;
    FtpEngineContext& operator=(FtpEngineContext const&) = delete;
    FtpEngineContext(FtpEngineContext&&) = delete;
    FtpEngineContext& operator=(FtpEngineContext&&) = delete;

    /**
     * @brief The executor the engines of the sessions make their strands on.
     */
    boost::asio::any_io_executor executor() const;

    std::shared_ptr<FileTransferProtocol::Resolver> resolver() const;

    std::size_t threads() const;

    /**
     * @brief Whether the calling thread is an engine thread.
     */
    bool runningInThisThread() const;

  private:
    struct Implementation;

    std::unique_ptr<Implementation> impl_;
};
