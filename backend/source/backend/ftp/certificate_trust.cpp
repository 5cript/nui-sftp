#include <backend/ftp/certificate_trust.hpp>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <chrono>

namespace FtpConnect
{
    namespace
    {
        std::string utcTime(std::chrono::sys_seconds time)
        {
            return fmt::format("{:%Y-%m-%d %H:%M:%S} UTC", time);
        }
    }

    nlohmann::json toJson(TrustQuestion const& question)
    {
        auto const& certificate = question.certificate;
        return {
            {"host", question.host},
            {"port", question.port},
            {"subject", certificate.subject},
            {"issuer", certificate.issuer},
            {"notBefore", utcTime(certificate.notBefore)},
            {"notAfter", utcTime(certificate.notAfter)},
            {"subjectAlternativeNames", certificate.subjectAlternativeNames},
            {"publicKeySha256", FileTransferProtocol::formatDigest(certificate.publicKeySha256)},
            {"certificateSha256", FileTransferProtocol::formatDigest(certificate.certificateSha256)},
            {"verifyFailure", FileTransferProtocol::reasonsOf(certificate.verifyFailures)},
            {"pinnedKeyDiffers", question.pinnedKeyDiffers},
        };
    }
}
