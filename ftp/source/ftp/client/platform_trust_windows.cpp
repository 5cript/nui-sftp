#include <ftp/client/platform_trust.hpp>
#include <log/log.hpp>
#include "platform_answer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        struct CloseStore
        {
            void operator()(void* store) const noexcept
            {
                CertCloseStore(static_cast<HCERTSTORE>(store), 0);
            }
        };
        using StoreHandle = std::unique_ptr<void, CloseStore>;

        struct FreeCertificate
        {
            void operator()(CERT_CONTEXT const* certificate) const noexcept
            {
                CertFreeCertificateContext(certificate);
            }
        };
        using CertificateHandle = std::unique_ptr<CERT_CONTEXT const, FreeCertificate>;

        struct FreeEngine
        {
            void operator()(void* engine) const noexcept
            {
                CertFreeCertificateChainEngine(static_cast<HCERTCHAINENGINE>(engine));
            }
        };
        using EngineHandle = std::unique_ptr<void, FreeEngine>;

        struct FreeChain
        {
            void operator()(CERT_CHAIN_CONTEXT const* chain) const noexcept
            {
                CertFreeCertificateChain(chain);
            }
        };
        using ChainHandle = std::unique_ptr<CERT_CHAIN_CONTEXT const, FreeChain>;

        /**
         * @return Nothing when @p text is not UTF-8.
         */
        std::optional<std::wstring> wideOf(std::string const& text)
        {
            if (text.empty())
                return std::wstring{};
            const auto size = static_cast<int>(text.size());
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
            if (length <= 0)
                return std::nullopt;
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, wide.data(), length) != length)
                return std::nullopt;
            return wide;
        }

        /**
         * @brief A store in memory with @p certificates.
         *
         * @return Nothing when a certificate cannot be read.
         */
        std::optional<StoreHandle> storeOf(std::span<DerCertificate const> certificates)
        {
            StoreHandle store{CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)};
            if (!store)
                return std::nullopt;
            for (auto const& der : certificates)
            {
                if (CertAddEncodedCertificateToStore(
                        static_cast<HCERTSTORE>(store.get()),
                        X509_ASN_ENCODING,
                        der.data(),
                        static_cast<DWORD>(der.size()),
                        CERT_STORE_ADD_ALWAYS,
                        nullptr
                    ) == FALSE)
                    return std::nullopt;
            }
            return store;
        }

        /**
         * @brief The engine that builds a chain, and the anchors it keeps to.
         */
        struct ChainEngine
        {
            /** @brief Closed after the engine that uses it. */
            StoreHandle anchors{};
            /** @brief Empty for the engine of the current user, which trusts the anchors of the system. */
            EngineHandle engine{};
        };

        /**
         * @brief The engine of the current user, or one that trusts @p exclusiveAnchors alone.
         *
         * @return Nothing when the engine cannot be made.
         */
        std::optional<ChainEngine> engineFor(std::optional<std::vector<DerCertificate>> const& exclusiveAnchors)
        {
            if (!exclusiveAnchors)
                return ChainEngine{};
            auto anchors = storeOf(*exclusiveAnchors);
            if (!anchors)
                return std::nullopt;
            CERT_CHAIN_ENGINE_CONFIG config{};
            config.cbSize = sizeof(config);
            config.hExclusiveRoot = static_cast<HCERTSTORE>(anchors->get());
            HCERTCHAINENGINE created = nullptr;
            if (CertCreateCertificateChainEngine(&config, &created) == FALSE)
                return std::nullopt;
            return ChainEngine{.anchors = std::move(*anchors), .engine = EngineHandle{created}};
        }

        /**
         * @brief The chain from the first certificate of @p chain to an anchor of @p engine, for a TLS server.
         *
         * @return The chain with what the engine found wrong with it. Nothing when it cannot be built at all.
         */
        ChainHandle builtChainOf(std::vector<DerCertificate> const& chain, ChainEngine const& engine)
        {
            auto const& leafDer = chain.front();
            const CertificateHandle leaf{CertCreateCertificateContext(
                X509_ASN_ENCODING, leafDer.data(), static_cast<DWORD>(leafDer.size())
            )};
            const auto sent = storeOf(std::span{chain}.subspan(1u));
            if (!leaf || !sent)
                return nullptr;
            std::array<char, sizeof(szOID_PKIX_KP_SERVER_AUTH)> serverAuthentication{szOID_PKIX_KP_SERVER_AUTH};
            std::array<LPSTR, 1> usages{serverAuthentication.data()};
            CERT_CHAIN_PARA parameters{};
            parameters.cbSize = sizeof(parameters);
            parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
            parameters.RequestedUsage.Usage.cUsageIdentifier = static_cast<DWORD>(usages.size());
            parameters.RequestedUsage.Usage.rgpszUsageIdentifier = usages.data();
            CERT_CHAIN_CONTEXT const* built = nullptr;
            if (CertGetCertificateChain(
                    static_cast<HCERTCHAINENGINE>(engine.engine.get()),
                    leaf.get(),
                    nullptr,
                    static_cast<HCERTSTORE>(sent->get()),
                    &parameters,
                    0,
                    nullptr,
                    &built
                ) == FALSE)
                return nullptr;
            return ChainHandle{built};
        }

        /**
         * @brief Whether the system trusts @p chain for the TLS server @p serverName. May fetch certificates, so it
         *        blocks for as long as the system waits for them.
         */
        bool systemTrusts(
            std::vector<DerCertificate> const& chain,
            std::wstring serverName,
            std::optional<std::vector<DerCertificate>> const& exclusiveAnchors
        )
        {
            // An engine takes an empty store of exclusive anchors as none and trusts the system's.
            if (exclusiveAnchors && exclusiveAnchors->empty())
                return false;
            const auto engine = engineFor(exclusiveAnchors);
            if (!engine)
            {
                Log::error("FTP: The system could not prepare the certificate check: error {}.", GetLastError());
                return false;
            }
            const auto built = builtChainOf(chain, *engine);
            if (!built)
            {
                Log::error("FTP: The system could not build the certificate chain: error {}.", GetLastError());
                return false;
            }
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA server{};
            server.cbSize = sizeof(server);
            server.dwAuthType = AUTHTYPE_SERVER;
            server.pwszServerName = serverName.data();
            CERT_CHAIN_POLICY_PARA policy{};
            policy.cbSize = sizeof(policy);
            policy.pvExtraPolicyPara = &server;
            CERT_CHAIN_POLICY_STATUS status{};
            status.cbSize = sizeof(status);
            if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, built.get(), &policy, &status) == FALSE)
            {
                Log::error("FTP: The system could not check the certificate chain: error {}.", GetLastError());
                return false;
            }
            if (status.dwError != 0u)
                Log::info("FTP: The system does not trust the certificate: error {:#010x}.", status.dwError);
            return status.dwError == 0u;
        }
    }

    PlatformVerdict platformVerdictOf(
        std::vector<DerCertificate> const& chain,
        std::string const& serverName,
        std::stop_token const& stop,
        std::optional<std::vector<DerCertificate>> const& exclusiveAnchors
    )
    {
        const auto name = wideOf(serverName);
        if (chain.empty() || !name)
            return PlatformVerdict::Refused;
        const auto answer = std::make_shared<Detail::PlatformAnswer>();
        // The check cannot be stopped: a thread of its own ends with it, the wait ends on a stop request.
        std::thread{
            [answer, chain, serverName = *name, exclusiveAnchors]() mutable
            {
                answer->give(systemTrusts(chain, std::move(serverName), exclusiveAnchors));
            }
        }.detach();
        const auto trusted = answer->waitUntilGiven(stop);
        return trusted && *trusted ? PlatformVerdict::Trusted : PlatformVerdict::Refused;
    }
}
