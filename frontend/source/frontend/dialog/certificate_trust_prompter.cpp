#include <frontend/dialog/certificate_trust_prompter.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/dialog.hpp>

#include <nui/frontend/api/json.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/val.hpp>
#include <nui/rpc.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Snc = ScriptNuiComponents;

namespace
{
    /**
     * @brief One reason the certificate was not verified: its kind, and the reason in the words of OpenSSL.
     */
    struct VerifyFailure
    {
        std::string kind{};
        std::string reason{};
    };

    /**
     * @brief The certificate the backend asks about, see FtpConnect::toJson(). Only shown: the answer names the
     *        prompt and a decision, nothing of the certificate.
     */
    struct TrustQuestion
    {
        int promptId{};
        std::string host{};
        std::uint16_t port{};
        std::string subject{};
        std::string issuer{};
        std::string notBefore{};
        std::string notAfter{};
        std::vector<std::string> subjectAlternativeNames{};
        std::string publicKeySha256{};
        std::string certificateSha256{};
        std::vector<VerifyFailure> verifyFailures{};
        bool pinnedKeyDiffers{false};
    };

    std::vector<VerifyFailure> failuresOf(nlohmann::json const& json)
    {
        std::vector<VerifyFailure> failures{};
        for (auto const& failure : json)
        {
            failures.push_back({
                .kind = failure.at("kind").get<std::string>(),
                .reason = failure.at("reason").get<std::string>(),
            });
        }
        return failures;
    }

    /**
     * @return Nothing when a member is missing or of another type.
     */
    std::optional<TrustQuestion> questionOf(Nui::val const& value)
    {
        try
        {
            const auto json = nlohmann::json::parse(Nui::JSON::stringify(value));
            return TrustQuestion{
                .promptId = json.at("promptId").get<int>(),
                .host = json.at("host").get<std::string>(),
                .port = json.at("port").get<std::uint16_t>(),
                .subject = json.at("subject").get<std::string>(),
                .issuer = json.at("issuer").get<std::string>(),
                .notBefore = json.at("notBefore").get<std::string>(),
                .notAfter = json.at("notAfter").get<std::string>(),
                .subjectAlternativeNames = json.at("subjectAlternativeNames").get<std::vector<std::string>>(),
                .publicKeySha256 = json.at("publicKeySha256").get<std::string>(),
                .certificateSha256 = json.at("certificateSha256").get<std::string>(),
                .verifyFailures = failuresOf(json.at("verifyFailures")),
                .pinnedKeyDiffers = json.at("pinnedKeyDiffers").get<bool>(),
            };
        }
        catch (std::exception const& exception)
        {
            Log::error("The trust prompt cannot read the certificate it is asked about: {}", exception.what());
            return std::nullopt;
        }
    }

    constexpr char const* refuse = "refuse";
    constexpr char const* trustOnce = "once";
    constexpr char const* trustAlways = "always";

    /**
     * @brief Answers the prompt @p promptId with @p decision: refuse, trustOnce or trustAlways. The backend trusts the
     *        key of the certificate it asked about, the answer names none.
     */
    void answer(int promptId, char const* decision)
    {
        Nui::RpcClient::call("CertificateTrustPrompter::promptDone", promptId, std::string{decision});
    }

    /**
     * @brief Refuses the prompt of @p value, a question that cannot be read. Without a prompt id nothing is answered.
     */
    void refuseUnread(Nui::val const& value)
    {
        const auto promptId = value["promptId"];
        if (promptId.isNumber())
            return answer(promptId.as<int>(), refuse);
        Log::error("The trust prompt names no prompt to answer.");
    }

    bool hasFailureOf(TrustQuestion const& question, std::string_view kind)
    {
        return std::ranges::any_of(
            question.verifyFailures,
            [kind](VerifyFailure const& failure)
            {
                return failure.kind == kind;
            }
        );
    }

    /**
     * @brief What the prompt says first: that the key changed, that the certificate names another server, or that it
     *        could not be verified.
     */
    std::string promptText(TrustQuestion const& question)
    {
        const auto key = [&question]()
        {
            if (question.pinnedKeyDiffers)
                return "textKeyChanged";
            if (hasFailureOf(question, "nameMismatch"))
                return "textNameMismatch";
            return "textUnverified";
        }();
        return fmt::format(
            fmt::runtime(language->get("certificateTrustPrompter", key)),
            fmt::format("{}:{}", question.host, question.port)
        );
    }

    /**
     * @brief The language key of the sentence for a failure of @p kind, nothing for a kind without one.
     */
    std::optional<char const*> problemKey(std::string_view kind)
    {
        if (kind == "nameMismatch")
            return "problemNameMismatch";
        if (kind == "expired")
            return "problemExpired";
        if (kind == "notYetValid")
            return "problemNotYetValid";
        if (kind == "selfSigned")
            return "problemSelfSigned";
        if (kind == "unknownAuthority")
            return "problemUnknownAuthority";
        return std::nullopt;
    }

    /**
     * @brief Every problem of the certificate in a sentence, each once. A failure of a kind without a sentence reads
     *        in the words of OpenSSL. A certificate without failures was refused for its key alone.
     */
    std::vector<std::string> problemsOf(TrustQuestion const& question)
    {
        std::vector<std::string> problems{};
        const auto add = [&problems](std::string problem)
        {
            if (std::ranges::find(problems, problem) == problems.end())
                problems.push_back(std::move(problem));
        };
        for (auto const& failure : question.verifyFailures)
        {
            const auto key = problemKey(failure.kind);
            add(key ? language->get("certificateTrustPrompter", *key) : failure.reason);
        }
        if (problems.empty())
            add(language->get("certificateTrustPrompter", "problemKeyOnly"));
        return problems;
    }

    std::string joined(std::vector<std::string> const& names)
    {
        return fmt::format("{}", fmt::join(names, ", "));
    }

    /**
     * @brief @p digest as formatDigest() writes it, in groups of eight pairs that a line breaks only between, so no
     *        pair is split. Copied, it reads as one.
     */
    Nui::ElementRenderer fingerprint(std::string const& digest)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;

        // Eight pairs with their colons.
        constexpr std::size_t groupLength = 24u;
        const auto group = [&digest](std::size_t index)
        {
            const auto start = std::min(index * groupLength, digest.size());
            return span{class_ = "certificate-trust-prompt-fingerprint-group"}(digest.substr(start, groupLength));
        };
        return span{
            class_ = "certificate-trust-prompt-fingerprint"
        }(group(0u), wbr{}(), group(1u), wbr{}(), group(2u), wbr{}(), group(3u));
    }
}

struct CertificateTrustPrompter::Implementation
{
    std::unique_ptr<Snc::Dialog> dialog{};
    Nui::Observed<std::optional<TrustQuestion>> question{};

    Nui::ElementRenderer body();
    void open(TrustQuestion trustQuestion);
};

Nui::ElementRenderer CertificateTrustPrompter::Implementation::body()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return section{
        class_ = "certificate-trust-prompt",
    }(
        observe(question),
        [this]() -> Nui::ElementRenderer {
            if (!question.value())
                return Nui::nil();
            auto const& shown = *question.value();
            const auto label = [](char const* labelKey) {
                return span{class_ = "certificate-trust-prompt-label"}(
                    language->get("certificateTrustPrompter", labelKey)
                );
            };
            const auto row = [&label](char const* labelKey, std::string const& value) {
                return fragment(label(labelKey), span{class_ = "certificate-trust-prompt-value"}(value));
            };
            return fragment(
                p{class_ = "certificate-trust-prompt-text"}(promptText(shown)),
                ul{class_ = "certificate-trust-prompt-problems"}(
                    Nui::range(problemsOf(shown)),
                    [](long long, std::string const& problem) -> Nui::ElementRenderer {
                        return li{}(problem);
                    }
                ),
                p{class_ = "certificate-trust-prompt-text"}(language->get("certificateTrustPrompter", "advice")),
                div{class_ = "certificate-trust-prompt-details"}(
                    row("subject", shown.subject),
                    row("issuer", shown.issuer),
                    row("validFrom", shown.notBefore),
                    row("validUntil", shown.notAfter),
                    row("alternativeNames", joined(shown.subjectAlternativeNames)),
                    label("publicKey"),
                    fingerprint(shown.publicKeySha256),
                    label("certificate"),
                    fingerprint(shown.certificateSha256)
                )
            );
        }
    );
    // clang-format on
}

void CertificateTrustPrompter::Implementation::open(TrustQuestion trustQuestion)
{
    Log::info("Asking whether to trust the certificate of {}:{}", trustQuestion.host, trustQuestion.port);
    const auto header = trustQuestion.pinnedKeyDiffers ? "headerKeyChanged" : "header";
    const auto promptId = trustQuestion.promptId;
    question = std::move(trustQuestion);
    Nui::globalEventContext.executeActiveEventsImmediately();

    // The dialog shows Ok before Yes, so the lasting choice comes first, as in other clients.
    auto labels = localizedButtonLabels();
    labels.ok = language->get("certificateTrustPrompter", "alwaysTrust");
    labels.yes = language->get("certificateTrustPrompter", "trustOnce");
    dialog->setButtonLabels(labels);
    dialog->open({
        .styleVariant = Snc::StyleVariant::Warning,
        .headerText = language->get("certificateTrustPrompter", header),
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Yes | Snc::Dialog::Button::Cancel,
        .initialFocus = Snc::Dialog::Button::Cancel,
        .onClose =
            [promptId](std::optional<Snc::Dialog::Button> button)
        {
            if (!button || *button == Snc::Dialog::Button::Cancel)
                return answer(promptId, refuse);
            answer(promptId, *button == Snc::Dialog::Button::Ok ? trustAlways : trustOnce);
        },
        .modal = true,
        .mayCloseWithoutButton = true,
    });
}

CertificateTrustPrompter::CertificateTrustPrompter()
    : impl_{std::make_unique<Implementation>()}
{
    impl_->dialog = std::make_unique<Snc::Dialog>("CertificateTrustPrompter", impl_->body());
    Nui::RpcClient::registerFunction(
        "CertificateTrustPrompter::prompt",
        [implementation = impl_.get()](Nui::val value)
        {
            auto trustQuestion = questionOf(value);
            if (!trustQuestion)
                return refuseUnread(value);
            implementation->open(*std::move(trustQuestion));
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(CertificateTrustPrompter);

Nui::ElementRenderer CertificateTrustPrompter::dialog()
{
    return (*impl_->dialog)();
}
