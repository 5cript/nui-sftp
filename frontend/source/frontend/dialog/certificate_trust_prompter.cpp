#include <frontend/dialog/certificate_trust_prompter.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <log/log.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state_holder.hpp>
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
#include <utility>
#include <variant>
#include <vector>

namespace Snc = ScriptNuiComponents;

namespace
{
    /**
     * @brief The certificate the backend asks about, see FtpConnect::toJson().
     */
    struct TrustQuestion
    {
        std::string host{};
        std::uint16_t port{};
        std::string subject{};
        std::string issuer{};
        std::string notBefore{};
        std::string notAfter{};
        std::vector<std::string> subjectAlternativeNames{};
        std::string publicKeySha256{};
        std::string certificateSha256{};
        std::string verifyFailure{};
        bool pinnedKeyDiffers{false};
    };

    /**
     * @return Nothing when a member is missing or of another type.
     */
    std::optional<TrustQuestion> questionOf(Nui::val const& value)
    {
        try
        {
            const auto json = nlohmann::json::parse(Nui::JSON::stringify(value));
            return TrustQuestion{
                .host = json.at("host").get<std::string>(),
                .port = json.at("port").get<std::uint16_t>(),
                .subject = json.at("subject").get<std::string>(),
                .issuer = json.at("issuer").get<std::string>(),
                .notBefore = json.at("notBefore").get<std::string>(),
                .notAfter = json.at("notAfter").get<std::string>(),
                .subjectAlternativeNames = json.at("subjectAlternativeNames").get<std::vector<std::string>>(),
                .publicKeySha256 = json.at("publicKeySha256").get<std::string>(),
                .certificateSha256 = json.at("certificateSha256").get<std::string>(),
                .verifyFailure = json.at("verifyFailure").get<std::string>(),
                .pinnedKeyDiffers = json.at("pinnedKeyDiffers").get<bool>(),
            };
        }
        catch (std::exception const& exception)
        {
            Log::error("The trust prompt cannot read the certificate it is asked about: {}", exception.what());
            return std::nullopt;
        }
    }

    void answer(std::string const& trustedKey)
    {
        Nui::RpcClient::call("CertificateTrustPrompter::promptDone", trustedKey);
    }

    /**
     * @brief Pins the key of @p question in every saved FTP session of its server with TLS.
     */
    void rememberTrust(Persistence::StateHolder& stateHolder, TrustQuestion const& question)
    {
        stateHolder.loadModifySave(
            [host = question.host, port = question.port, key = question.publicKeySha256](Persistence::State& state)
            {
                for (auto& [name, options] : state.sessions)
                {
                    auto* ftp = std::get_if<Persistence::FtpSessionOptions>(&options.engine);
                    if (!ftp || ftp->host != host || ftp->effectivePort() != port ||
                        ftp->tlsMode == Persistence::FtpTlsMode::Plain)
                        continue;
                    Log::info("Pinned the trusted public key of {}:{} in the session '{}'.", host, port, name);
                    ftp->pinnedPublicKeySha256 = key;
                }
            },
            [](std::optional<std::string> const& error)
            {
                if (error)
                    Log::error("The trusted public key could not be saved: {}", *error);
            }
        );
    }

    std::string promptText(TrustQuestion const& question)
    {
        const auto key = question.pinnedKeyDiffers ? "textKeyChanged" : "text";
        return fmt::format(
            fmt::runtime(language->get("certificateTrustPrompter", key)),
            fmt::format("{}:{}", question.host, question.port)
        );
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
    Persistence::StateHolder* stateHolder;
    std::unique_ptr<Snc::Dialog> dialog{};
    Nui::Observed<std::optional<TrustQuestion>> question{};

    explicit Implementation(Persistence::StateHolder& stateHolder)
        : stateHolder{&stateHolder}
    {}

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
                div{class_ = "certificate-trust-prompt-details"}(
                    row("reason", shown.verifyFailure),
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
            [this](std::optional<Snc::Dialog::Button> button)
        {
            auto const& asked = question.value();
            if (!asked || !button || *button == Snc::Dialog::Button::Cancel)
                return answer("");
            if (*button == Snc::Dialog::Button::Ok)
                rememberTrust(*stateHolder, *asked);
            answer(asked->publicKeySha256);
        },
        .modal = true,
        .mayCloseWithoutButton = true,
    });
}

CertificateTrustPrompter::CertificateTrustPrompter(Persistence::StateHolder& stateHolder)
    : impl_{std::make_unique<Implementation>(stateHolder)}
{
    impl_->dialog = std::make_unique<Snc::Dialog>("CertificateTrustPrompter", impl_->body());
    Nui::RpcClient::registerFunction(
        "CertificateTrustPrompter::prompt",
        [implementation = impl_.get()](Nui::val value)
        {
            auto trustQuestion = questionOf(value);
            if (!trustQuestion)
                return answer("");
            implementation->open(*std::move(trustQuestion));
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(CertificateTrustPrompter);

Nui::ElementRenderer CertificateTrustPrompter::dialog()
{
    return (*impl_->dialog)();
}
