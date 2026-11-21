#pragma once

#include <ftp/client/tls.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace FtpConnect
{
    /**
     * @brief A public key the user trusts for one FTP server.
     */
    struct TrustedKey
    {
        FileTransferProtocol::Sha256Digest publicKey{};
        /** @brief Saved, so later runs of the application trust it too. */
        bool lasting{false};
    };

    /**
     * @brief The public keys the user trusts for FTP servers, by host and port: the ones trusted for this run of the
     *        application and the saved ones. A key gets in here only from a certificate the backend saw itself, the
     *        frontend can show a key and have it forgotten, never name one. Thread-safe.
     */
    class CertificateTrustStore
    {
      public:
        /**
         * @brief Trusts keys for this run only, nothing is loaded or saved.
         */
        CertificateTrustStore() = default;

        /**
         * @brief Loads the saved keys of @p file and saves there. A file that cannot be read is logged and kept as
         *        "<file>.unreadable", no key of it is trusted.
         */
        explicit CertificateTrustStore(std::filesystem::path file);
        ~CertificateTrustStore() = default;
        CertificateTrustStore(CertificateTrustStore const&) = delete;
        CertificateTrustStore& operator=(CertificateTrustStore const&) = delete;
        CertificateTrustStore(CertificateTrustStore&&) = delete;
        CertificateTrustStore& operator=(CertificateTrustStore&&) = delete;

        /**
         * @brief The key trusted for the server, the one of this run before a saved one. Host names compare without
         *        case.
         */
        std::optional<TrustedKey> trustedKeyOf(std::string_view host, std::uint16_t port) const;

        /**
         * @brief Trusts @p publicKey for the server until the application ends, a saved key of the server stays saved.
         */
        void
        trustForThisRun(std::string_view host, std::uint16_t port, FileTransferProtocol::Sha256Digest const& publicKey);

        /**
         * @brief Trusts @p publicKey for the server and saves it, in place of the key saved for it before.
         *
         * @return Why it could not be saved. The key is trusted for this run then.
         */
        std::expected<void, std::string>
        trustLastingly(std::string_view host, std::uint16_t port, FileTransferProtocol::Sha256Digest const& publicKey);

        /**
         * @brief Trusts no key for the server any more, neither for this run nor saved.
         *
         * @return Why the saved keys could not be written. The key is forgotten for this run then.
         */
        std::expected<void, std::string> forget(std::string_view host, std::uint16_t port);

      private:
        struct Server
        {
            std::string host{};
            std::uint16_t port{};

            auto operator<=>(Server const&) const = default;
        };
        using Keys = std::map<Server, FileTransferProtocol::Sha256Digest>;

        static Server serverOf(std::string_view host, std::uint16_t port);

        /**
         * @return Why the file could not be read, nothing is loaded then.
         */
        std::expected<Keys, std::string> load() const;

        /**
         * @brief Writes the saved keys beside the file and moves them over it. With the mutex held.
         */
        std::expected<void, std::string> save() const;

      private:
        /** @brief Empty when nothing is saved. */
        std::filesystem::path file_{};
        mutable std::mutex mutex_{};
        Keys lasting_{};
        Keys thisRun_{};
    };
}
