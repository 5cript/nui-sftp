#pragma once

#include <remote_io/remote_error.hpp>
#include "client_support.hpp"

#include <fmt/format.h>
#include <openssl/err.h>

#include <array>
#include <string>
#include <string_view>

namespace FileTransferProtocol::Detail
{
    /**
     * @brief The oldest error OpenSSL queued, the queue is empty afterwards.
     */
    inline std::string openSslReason()
    {
        const auto code = ERR_get_error();
        ERR_clear_error();
        if (code == 0u)
            return "no reason given";
        std::array<char, 256> reason{};
        ERR_error_string_n(code, reason.data(), reason.size());
        return reason.data();
    }

    /**
     * @brief A Failure for @p what, which OpenSSL refused, with its reason.
     */
    inline RemoteIo::RemoteError refusedByOpenSsl(std::string_view what)
    {
        return errorOf(
            RemoteIo::RemoteErrorKind::Failure, fmt::format("OpenSSL refused {}: {}", what, openSslReason())
        );
    }
}
