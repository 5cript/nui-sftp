#pragma once

#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <optional>

namespace FileTransferProtocol
{
    /**
     * @brief What a reply that did not succeed means, as a RemoteError with the code and the text of the reply.
     *
     * Codes the client does not know get their kind from the first digit (RFC 1123 4.1.2.11). 450, 451, 550 and 553
     * say too little alone: an errno text such as "No such file or directory" decides between missing, denied and
     * existing, otherwise the kind is Failure (553 PermissionDenied) and the caller asks the server again when it needs
     * to know. 421 and 530 with a text about too many connections are ConnectionLimit.
     *
     * @param verb The command the reply answers, nothing for the greeting.
     * @return A ProtocolViolation for a positive reply, which the caller did not expect when it asks.
     */
    RemoteIo::RemoteError replyError(Reply const& reply, std::optional<Verb> verb);
}
