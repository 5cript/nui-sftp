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
     * Codes the client does not know get their kind from the first digit (RFC 1123 4.1.2.11). 450, 451, 521, 550 and
     * 553 say too little alone: an errno or fixed text such as "No such file or directory" at the end of a line
     * decides between missing, denied and existing, words of an echoed path before it do not. Otherwise the kind is
     * Failure (553 PermissionDenied) and the caller asks the server again when it needs to know. 421 and 530 with a
     * text about too many connections are ConnectionLimit, about too many failed logins AuthenticationFailed.
     *
     * @param verb The command the reply answers, nothing for the greeting.
     * @return A ProtocolViolation for a positive reply, which the caller did not expect when it asks.
     */
    RemoteIo::RemoteError replyError(Reply const& reply, std::optional<Verb> verb);

    /**
     * @brief A refused transfer whose reply fits a data connection that did not resume the TLS session of the control
     *        connection: a 522 (RFC 4217 10.2) as vsftpd sends it, a 425 or 450 that names the session as FileZilla
     *        Server does, or that ends in proftpd's "Operation not permitted". The reply alone proves nothing, the
     *        caller has to know that the data connection has TLS and resumed no session.
     */
    bool tellsOfMissingSessionReuse(Reply const& reply);
}
