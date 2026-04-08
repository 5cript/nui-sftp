#pragma once

#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <expected>
#include <functional>

#include <cstring>
#include <unistd.h>

/**
 * @brief Shared framing constants and stateless helpers used by both the
 *        boost-async and plain-fd JSON I/O classes.
 *
 *        Frame layout: MAGIC (12 bytes) | body-length LE-u32 (4 bytes) | body
 */
class JsonFraming
{
  public:
    static constexpr std::string_view MAGIC = "JSON_IO_HEAD";
    static constexpr std::size_t frameHeaderSize = 4u + MAGIC.size();

    using ErrorType = std::string;
    using ResultType = std::expected<nlohmann::json, ErrorType>;

    static std::uint32_t readU32LE(const char* buf) noexcept
    {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[0])) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[1])) << 8u) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[2])) << 16u) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[3])) << 24u);
    }

    static void writeU32LE(char* buf, std::uint32_t val) noexcept
    {
        buf[0] = static_cast<char>(val & 0xFFu);
        buf[1] = static_cast<char>((val >> 8u) & 0xFFu);
        buf[2] = static_cast<char>((val >> 16u) & 0xFFu);
        buf[3] = static_cast<char>((val >> 24u) & 0xFFu);
    }

  protected:
    /** @brief Serialise json into a length-framed wire buffer. */
    static std::string buildFrame(nlohmann::json const& json)
    {
        std::string payload = json.dump();
        std::string frame(frameHeaderSize + payload.size(), '\0');
        std::memcpy(frame.data(), MAGIC.data(), MAGIC.size());
        writeU32LE(frame.data() + MAGIC.size(), static_cast<std::uint32_t>(payload.size()));
        std::memcpy(frame.data() + frameHeaderSize, payload.data(), payload.size());
        return frame;
    }

    /**
     * @brief Parse all complete frames out of accum, calling onResult for each.
     * @param accum  Accumulation buffer; consumed bytes are erased on success.
     * @return false if a framing or JSON parse error occurred (onResult already
     *         called with the error); true otherwise.
     */
    static bool parseAccum(
        std::string& accum,
        std::function<void(ResultType const&)> const& onResult
    )
    {
        std::string_view view{accum};
        std::size_t totalConsumed = 0;

        while (view.size() >= frameHeaderSize)
        {
            if (!view.starts_with(MAGIC))
            {
                spdlog::error("Invalid frame header: '{}'", view.substr(0, MAGIC.size()));
                onResult(std::unexpected(std::string{"Invalid frame header"}));
                return false;
            }

            std::uint32_t bodyLen = readU32LE(view.data() + MAGIC.size());
            if (view.size() < frameHeaderSize + bodyLen)
                break; // incomplete frame — wait for more data

            std::string_view payload = view.substr(frameHeaderSize, bodyLen);
            view.remove_prefix(frameHeaderSize + bodyLen);
            totalConsumed += frameHeaderSize + bodyLen;

            spdlog::info("Received message: {}", payload);
            try
            {
                onResult(nlohmann::json::parse(payload));
            }
            catch (std::exception const& exc)
            {
                onResult(std::unexpected(std::string{"JSON parse error: "} + exc.what()));
                return false;
            }
        }

        if (totalConsumed == accum.size())
            accum.clear();
        else
            accum.erase(0, totalConsumed);

        return true;
    }
};

// ---------------------------------------------------------------------------

/**
 * @brief Async JSON framing over a pair of boost::asio streams.
 *        Used by the parent process side of ForkPool.
 */
template <typename InputStream, typename OutputStream>
class JsonProcessIo : public JsonFraming
{
  public:
    JsonProcessIo(InputStream input, OutputStream output)
        : input_{std::move(input)}
        , output_{std::move(output)}
        , readBufferCurrent_(4096, '\0')
    {}

    void write(nlohmann::json const& json, std::function<void(bool)> onSent)
    {
        std::shared_ptr<std::string> frame = std::make_shared<std::string>(buildFrame(json));
        spdlog::info("Sending message of size {}: {}", frame->size() - frameHeaderSize, frame->substr(frameHeaderSize));
        boost::asio::async_write(
            output_,
            boost::asio::buffer(*frame),
            [frame, onSent = std::move(onSent)](boost::system::error_code err, std::size_t bytes)
            {
                spdlog::debug("Sent {} byte(s), error: {}", bytes, err ? err.message() : "none");
                onSent(!err);
            }
        );
    }

    void enterReadLoop(std::function<void(ResultType const&)> onResult)
    {
        onResult_ = std::move(onResult);
        readOnce();
    }

    InputStream& input()
    {
        return input_;
    }
    OutputStream& output()
    {
        return output_;
    }

  private:
    void readOnce()
    {
        input_.async_read_some(
            boost::asio::buffer(readBufferCurrent_.data(), readBufferCurrent_.size()),
            [this](boost::system::error_code ec, std::size_t bytes_transferred)
            {
                spdlog::debug("Read {} bytes", bytes_transferred);
                if (ec)
                {
                    spdlog::error("Error reading from input: {}", ec.message());
                    onResult_(std::unexpected(ec.message()));
                    return;
                }
                spdlog::trace("Read data: {}", std::string_view(readBufferCurrent_.data(), bytes_transferred));
                readBufferAccum_.append(readBufferCurrent_, 0, bytes_transferred);
                if (!parseAccum(readBufferAccum_, onResult_))
                    return;
                readOnce();
            }
        );
    }

  private:
    InputStream input_;
    OutputStream output_;
    std::string readBufferCurrent_;
    std::string readBufferAccum_;
    std::function<void(ResultType const&)> onResult_{};
};

// ---------------------------------------------------------------------------

/**
 * @brief Synchronous JSON framing over plain file descriptors.
 *        Used by the worker process (no boost, no threads).
 *
 *        The caller drives readability via epoll_wait and calls readAvailable()
 *        exactly once per readable event.  writeJson() is a blocking write.
 */
class FdJsonIo : public JsonFraming
{
  public:
    FdJsonIo(int readFd, int writeFd)
        : readFd_{readFd}
        , writeFd_{writeFd}
    {}

    /**
     * @brief Blocking write of a length-framed JSON message.
     * @return true on success, false on I/O error.
     */
    bool writeJson(nlohmann::json const& json)
    {
        std::string frame = buildFrame(json);
        spdlog::info("[worker] sending {} byte(s): {}", frame.size() - frameHeaderSize, frame.substr(frameHeaderSize));

        const char* ptr = frame.data();
        std::size_t remaining = frame.size();
        while (remaining > 0)
        {
            ssize_t written = ::write(writeFd_, ptr, remaining);
            if (written > 0)
            {
                ptr += written;
                remaining -= static_cast<std::size_t>(written);
            }
            else if (written == -1 && errno == EINTR)
            {
                /* interrupted — retry */
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Read one batch of available bytes from the read fd and parse any
     *        complete frames, invoking onResult for each.
     *
     *        Must be called at most once per epoll readable event.
     *
     * @return false on EOF or unrecoverable error; true to keep polling.
     */
    bool readAvailable(std::function<void(ResultType const&)> const& onResult)
    {
        char buf[4096];
        ssize_t nread = ::read(readFd_, buf, sizeof(buf));
        if (nread > 0)
        {
            accum_.append(buf, static_cast<std::size_t>(nread));
            return parseAccum(accum_, onResult);
        }
        else if (nread == 0)
        {
            onResult(std::unexpected(std::string{"EOF on IPC pipe"}));
            return false;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return true; // spurious wakeup
            onResult(std::unexpected(std::string{std::strerror(errno)}));
            return false;
        }
    }

  private:
    int readFd_;
    int writeFd_;
    std::string accum_;
};
