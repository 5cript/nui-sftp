#include <backend/ftp/certificate_trust.hpp>
#include <log/log.hpp>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <chrono>
#include <optional>
#include <utility>

namespace FtpConnect
{
    namespace
    {
        using FileTransferProtocol::VerifyFailureKind;

        std::string utcTime(std::chrono::sys_seconds time)
        {
            return fmt::format("{:%Y-%m-%d %H:%M:%S} UTC", time);
        }

        std::string_view nameOf(VerifyFailureKind kind)
        {
            switch (kind)
            {
                case VerifyFailureKind::NameMismatch:
                    return "nameMismatch";
                case VerifyFailureKind::Expired:
                    return "expired";
                case VerifyFailureKind::NotYetValid:
                    return "notYetValid";
                case VerifyFailureKind::SelfSigned:
                    return "selfSigned";
                case VerifyFailureKind::UnknownAuthority:
                    return "unknownAuthority";
                case VerifyFailureKind::Other:
                    break;
            }
            return "other";
        }

        nlohmann::json toJson(std::vector<FileTransferProtocol::VerifyFailure> const& failures)
        {
            auto json = nlohmann::json::array();
            for (auto const& failure : failures)
                json.push_back({{"kind", nameOf(failure.kind)}, {"reason", failure.reason}});
            return json;
        }

        std::optional<TrustAnswer> answerOf(std::string_view decision)
        {
            if (decision == "once")
                return TrustAnswer::Once;
            if (decision == "always")
                return TrustAnswer::Always;
            if (decision == "refuse")
                return TrustAnswer::Refused;
            return std::nullopt;
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
            {"verifyFailures", toJson(certificate.verifyFailures)},
            {"pinnedKeyDiffers", question.pinnedKeyDiffers},
        };
    }

    int PendingTrustPrompt::begin(std::function<void(TrustAnswer)> onAnswer)
    {
        std::function<void(TrustAnswer)> waiting{};
        int promptId = 0;
        {
            std::scoped_lock lock{mutex_};
            waiting = std::exchange(onAnswer_, std::move(onAnswer));
            promptId = ++waitingId_;
        }
        if (waiting)
            waiting(TrustAnswer::Refused);
        return promptId;
    }

    bool PendingTrustPrompt::answer(int promptId, std::string_view decision)
    {
        std::function<void(TrustAnswer)> onAnswer{};
        {
            std::scoped_lock lock{mutex_};
            if (!onAnswer_ || promptId != waitingId_)
            {
                Log::warn("An answer to the trust prompt {} came while it did not wait, it is dropped.", promptId);
                return false;
            }
            onAnswer = std::exchange(onAnswer_, {});
        }
        const auto answer = answerOf(decision);
        if (!answer)
            Log::error("The trust prompt was answered with no decision, the certificate is not trusted.");
        onAnswer(answer.value_or(TrustAnswer::Refused));
        return true;
    }
}
