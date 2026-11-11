#pragma once

#include <ftp/client/transfer.hpp>
#include <ftp/protocol/listing.hpp>
#include <remote_io/remote_error.hpp>
#include <shared_data/directory_entry.hpp>

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    /**
     * @brief Whether a server takes a command, learned from the first reply that tells.
     */
    enum class Support : std::uint8_t
    {
        Unknown,
        Works,
        Refused,
    };

    /**
     * @brief What a session learned about the commands of its server, shared by its connections on any strand. FEAT
     *        tells too little: vsftpd answers MLSD with 500, proftpd takes MLSD without listing it. Thread-safe.
     */
    class ServerDialect
    {
      public:
        /** @brief MLSD, RFC 3659 7, asked only where LIST has lines the parser cannot read. */
        Support machineListing() const;
        void learnMachineListing(Support support);

        /** @brief LIST -a, which lists hidden entries where the server reads the option and a path "-a" elsewhere. */
        Support listAll() const;
        void learnListAll(Support support);

        /** @brief STAT with a path, which lists it like ls -l on the control connection (RFC 959 4.1.3). */
        Support status() const;
        void learnStatus(Support support);

      private:
        std::atomic<Support> machineListing_{Support::Unknown};
        std::atomic<Support> listAll_{Support::Unknown};
        std::atomic<Support> status_{Support::Unknown};
    };

    /**
     * @brief What a metadata operation runs on: a logged-in control connection, the caller's alone until the operation
     *        ended, and what the session learned. The operations run on the strand of the control connection and leave
     *        its working directory changed, they send absolute paths or change into the directory they list.
     */
    struct MetadataSession
    {
        TransferSession transfer{};
        std::shared_ptr<ServerDialect> dialect = std::make_shared<ServerDialect>();
        TransferOptions options{};
        ListingLimits limits{};
    };

    /**
     * @brief The entries of the directory @p path: CWD into it, so a name is never read as an option or a pattern,
     *        then LIST -a where the server reads the option, else LIST. LIST shows every symlink with its target on
     *        every server, where MLSD leaves dangling links out on proftpd and pyftpdlib and gives links no target;
     *        its times have minute precision. MLSD only lists a directory again whose LIST had lines the parser
     *        cannot read. A server that lists a directory it cannot enter as empty cannot fool the CWD.
     *
     * @return The listing. The replyError() of the CWD or of the listing, errors as for fetchListing().
     */
    boost::asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>>
    listDirectory(MetadataSession session, std::string path);

    /**
     * @brief What the absolute @p path is, itself when it is a symlink: the one line STAT lists for a file or a link,
     *        else its entry in the LIST of its parent. STAT lists the content of a directory, and proftpd's STAT
     *        follows links, so it misses dangling ones: only the parent tells that a path does not exist. Where the
     *        parent cannot be entered and its refusal does not say why, as with vsftpd, the parent is looked up the
     *        same way.
     *
     * @return The entry, named like the last segment of @p path. NoSuchFile for a path that does not exist, also below
     *         a missing directory or a file; PermissionDenied below a directory that cannot be listed; the error of a
     *         broken connection.
     */
    boost::asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    lstatPath(MetadataSession session, std::string path);

    /**
     * @brief What the absolute @p path is, the end of the chain when it is a symlink: SIZE and MDTM for a file, then
     *        CWD for a directory, which all follow links. Not MLST: proftpd's follows a link for its type but gives
     *        the size of the link. When none of them answers, lstatPath() decides between a missing path and one that
     *        cannot be entered.
     *
     * @return The entry, named like the last segment of @p path, a directory with no more than its type. NoSuchFile
     *         also for a link whose target cannot be reached; else errors as for lstatPath().
     */
    boost::asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    statPath(MetadataSession session, std::string path);

    /**
     * @brief @p entry as the rest of the application reads entries. A name that is no UTF-8 is shown with U+FFFD and
     *        marked, owner, group and link target are made UTF-8. A time from LIST is taken as UTC with seconds as
     *        listed, the listing names no time zone and vsftpd and proftpd print UTC.
     */
    SharedData::DirectoryEntry toDirectoryEntry(ListingEntry const& entry);
}
