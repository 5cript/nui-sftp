#include <backend/ftp/certificate_trust_store.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/algorithm/case_convert.hpp>
#include <utility/error_message.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace FtpConnect
{
    namespace
    {
        using FileTransferProtocol::Sha256Digest;

        std::filesystem::path besideOf(std::filesystem::path const& file, std::string_view suffix)
        {
            auto beside = file;
            beside += Paths::nativeFromUtf8(std::string{suffix});
            return beside;
        }

        std::string describe(std::filesystem::path const& file)
        {
            return Paths::nativeToUtf8(file);
        }
    }

    CertificateTrustStore::CertificateTrustStore(std::filesystem::path file)
        : file_{std::move(file)}
    {
        auto loaded = load();
        if (loaded)
        {
            lasting_ = std::move(*loaded);
            return;
        }
        Log::error("The trusted keys of FTP servers could not be read, none is trusted: {}", loaded.error());
        std::error_code copyError{};
        std::filesystem::copy_file(
            file_, besideOf(file_, ".unreadable"), std::filesystem::copy_options::overwrite_existing, copyError
        );
        if (copyError)
            Log::error("The unreadable file {} could not be kept: {}", describe(file_), copyError.message());
    }

    CertificateTrustStore::Server CertificateTrustStore::serverOf(std::string_view host, std::uint16_t port)
    {
        return Server{.host = Utility::Algorithm::toLowerCase(std::string{host}), .port = port};
    }

    std::optional<TrustedKey> CertificateTrustStore::trustedKeyOf(std::string_view host, std::uint16_t port) const
    {
        const auto server = serverOf(host, port);
        std::scoped_lock lock{mutex_};
        if (const auto found = thisRun_.find(server); found != thisRun_.end())
            return TrustedKey{.publicKey = found->second, .lasting = false};
        if (const auto found = lasting_.find(server); found != lasting_.end())
            return TrustedKey{.publicKey = found->second, .lasting = true};
        return std::nullopt;
    }

    void
    CertificateTrustStore::trustForThisRun(std::string_view host, std::uint16_t port, Sha256Digest const& publicKey)
    {
        std::scoped_lock lock{mutex_};
        thisRun_.insert_or_assign(serverOf(host, port), publicKey);
    }

    std::expected<void, std::string>
    CertificateTrustStore::trustLastingly(std::string_view host, std::uint16_t port, Sha256Digest const& publicKey)
    {
        const auto server = serverOf(host, port);
        std::scoped_lock lock{mutex_};
        const auto before = lasting_;
        lasting_.insert_or_assign(server, publicKey);
        thisRun_.erase(server);
        auto saved = save();
        if (!saved)
        {
            lasting_ = before;
            thisRun_.insert_or_assign(server, publicKey);
        }
        return saved;
    }

    std::expected<void, std::string> CertificateTrustStore::forget(std::string_view host, std::uint16_t port)
    {
        const auto server = serverOf(host, port);
        std::scoped_lock lock{mutex_};
        thisRun_.erase(server);
        if (lasting_.erase(server) == 0u)
            return {};
        return save();
    }

    std::expected<CertificateTrustStore::Keys, std::string> CertificateTrustStore::load() const
    {
        std::error_code existsError{};
        if (!std::filesystem::exists(file_, existsError) && !existsError)
            return Keys{};
        try
        {
            std::ifstream reader{file_, std::ios_base::binary};
            if (!reader.good())
                return std::unexpected(fmt::format("{} cannot be opened", describe(file_)));
            const auto json = nlohmann::json::parse(reader);
            Keys keys{};
            for (auto const& entry : json.at("servers"))
            {
                const auto port = entry.at("port").get<int>();
                const auto key = FileTransferProtocol::parseDigest(entry.at("publicKeySha256").get<std::string>());
                if (!key || port < 1 || port > std::numeric_limits<std::uint16_t>::max())
                    return std::unexpected(
                        fmt::format("{} holds an entry that names no key or no port", describe(file_))
                    );
                keys.insert_or_assign(
                    serverOf(entry.at("host").get<std::string>(), static_cast<std::uint16_t>(port)), *key
                );
            }
            return keys;
        }
        catch (std::exception const& exception)
        {
            return std::unexpected(fmt::format("{}: {}", describe(file_), Utility::errorMessage(exception)));
        }
    }

    std::expected<void, std::string> CertificateTrustStore::save() const
    {
        if (file_.empty())
            return {};
        auto servers = nlohmann::json::array();
        for (auto const& [server, key] : lasting_)
        {
            servers.push_back({
                {"host", server.host},
                {"port", server.port},
                {"publicKeySha256", FileTransferProtocol::formatDigest(key)},
            });
        }
        const auto beside = besideOf(file_, ".new");
        std::error_code error{};
        std::filesystem::create_directories(file_.parent_path(), error);
        {
            std::ofstream writer{beside, std::ios_base::binary | std::ios_base::trunc};
            writer << nlohmann::json{{"servers", std::move(servers)}}.dump(4);
            writer.flush();
            if (!writer.good())
                return std::unexpected(fmt::format("{} cannot be written", describe(beside)));
        }
        std::filesystem::rename(beside, file_, error);
        if (error)
            return std::unexpected(
                fmt::format("{} cannot take the place of {}: {}", describe(beside), describe(file_), error.message())
            );
        return {};
    }
}
