#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

/**
 * @brief Manages a dedicated fork-worker process, routing all child-process spawning
 *        through a single-threaded worker to sidestep the multithreaded-fork hazard on Linux.
 *
 *        Communication uses a pair of async pipes with 4-byte LE length-prefixed JSON frames.
 *
 *        Protocol (parent → worker):
 *          {"id":"…","command":"spawn","payload":{"exe":"…","args":[…],"env":{…},"termios":{…}}}
 *          {"id":"…","command":"stdin", "payload":{"data":"<base64>"}}
 *          {"id":"…","command":"resize","payload":{"cols":80,"rows":24}}
 *          {"id":"…","command":"kill"}
 *          {"command":"quit"}
 *
 *        Protocol (worker → parent):
 *          {"id":"…","type":"stdout","data":"<base64>"}
 *          {"id":"…","type":"exit",  "code":0}
 *          {"id":"…","type":"error", "message":"…"}
 */
class ForkPool
{
  public:
    ForkPool();
    ~ForkPool();

    ForkPool(ForkPool const&) = delete;
    ForkPool& operator=(ForkPool const&) = delete;
    ForkPool(ForkPool&&) noexcept;
    ForkPool& operator=(ForkPool&&) noexcept;

    /**
     * @brief Fork the worker process and open async communication pipes.
     * @param executor  Executor that drives async I/O on the parent side.
     * @param onMessage Invoked on the executor for each JSON message received from the worker.
     */
    void start(boost::asio::any_io_executor executor, std::function<void(nlohmann::json const&)> onMessage);

    /**
     * @brief Replace the message handler after start().  Thread-safe.
     * @param handler New callback invoked for each worker message.
     */
    void setMessageHandler(std::function<void(nlohmann::json const&)> handler);

    /** @brief Gracefully shut down the worker; blocks until the process exits. */
    void stop();

    /**
     * @brief Send a length-framed JSON message to the worker.
     * @param message Serialised JSON payload; framing is prepended automatically.
     */
    void send(nlohmann::json const& message);

  private:
    struct Implementation;
    std::shared_ptr<Implementation> impl_;
};
