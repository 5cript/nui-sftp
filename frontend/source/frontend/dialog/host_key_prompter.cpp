#include <frontend/dialog/host_key_prompter.hpp>
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
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Snc = ScriptNuiComponents;

namespace
{
    enum class HostKeyProblem
    {
        Unknown,
        Changed,
        OtherType,
        ChangedSinceTrusted,
    };

    /**
     * @brief The host key the backend asks about, see the backend's HostKeyPrompter. Only shown: the answer names the
     *        prompt and a decision, nothing of the key.
     */
    struct HostKeyQuestion
    {
        int promptId{};
        HostKeyProblem problem{HostKeyProblem::Unknown};
        /** @brief Whether the key is the one of a jump host on the way to the server. */
        bool jumpHost{false};
        std::string host{};
        int port{};
        std::string keyType{};
        std::string fingerprint{};
        std::string knownHostsFile{};
        /** @brief The key trusted before, of a key that is ChangedSinceTrusted. */
        std::string trustedKey{};
        /** @brief The command that removes the old key from the known hosts file, empty when there is none. */
        std::string removalCommand{};
    };

    /**
     * @return Nothing when @p name names no problem.
     */
    std::optional<HostKeyProblem> problemOf(std::string const& name)
    {
        if (name == "unknown")
            return HostKeyProblem::Unknown;
        if (name == "changed")
            return HostKeyProblem::Changed;
        if (name == "otherType")
            return HostKeyProblem::OtherType;
        if (name == "changedSinceTrusted")
            return HostKeyProblem::ChangedSinceTrusted;
        return std::nullopt;
    }

    /**
     * @return Nothing when a member is missing, of another type or names no problem.
     */
    std::optional<HostKeyQuestion> questionOf(Nui::val const& value)
    {
        try
        {
            const auto json = nlohmann::json::parse(Nui::JSON::stringify(value));
            const auto problem = problemOf(json.at("problem").get<std::string>());
            if (!problem)
            {
                Log::error("The host key prompt is asked about a problem it does not know.");
                return std::nullopt;
            }
            return HostKeyQuestion{
                .promptId = json.at("promptId").get<int>(),
                .problem = *problem,
                .jumpHost = json.at("jumpHost").get<bool>(),
                .host = json.at("host").get<std::string>(),
                .port = json.at("port").get<int>(),
                .keyType = json.at("keyType").get<std::string>(),
                .fingerprint = json.at("fingerprint").get<std::string>(),
                .knownHostsFile = json.at("knownHostsFile").get<std::string>(),
                .trustedKey = json.at("trustedKey").get<std::string>(),
                .removalCommand = json.at("removalCommand").get<std::string>(),
            };
        }
        catch (std::exception const& exception)
        {
            Log::error("The host key prompt cannot read the key it is asked about: {}", exception.what());
            return std::nullopt;
        }
    }

    constexpr char const* refuse = "refuse";
    constexpr char const* trustAlways = "always";

    /**
     * @brief Answers the prompt @p promptId with @p decision: refuse or trustAlways. The backend trusts the key the
     *        server presented, the answer names none.
     */
    void answer(int promptId, char const* decision)
    {
        Nui::RpcClient::call("HostKeyPrompter::promptDone", promptId, std::string{decision});
    }

    /**
     * @brief Refuses the prompt of @p value, a question that cannot be read. Without a prompt id nothing is answered.
     */
    void refuseUnread(Nui::val const& value)
    {
        const auto promptId = value["promptId"];
        if (promptId.isNumber())
            return answer(promptId.as<int>(), refuse);
        Log::error("The host key prompt names no prompt to answer.");
    }

    std::string serverOf(HostKeyQuestion const& question)
    {
        return fmt::format("{}:{}", question.host, question.port);
    }

    /**
     * @brief The text of @p key with the server of @p question in it.
     */
    std::string textAbout(HostKeyQuestion const& question, char const* key)
    {
        return fmt::format(fmt::runtime(language->get("hostKeyPrompter", key)), serverOf(question));
    }

    Nui::ElementRenderer paragraph(std::string const& content, char const* className = "host-key-prompt-text")
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        return p{class_ = className}(content);
    }

    Nui::ElementRenderer label(char const* labelKey)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;
        return span{class_ = "host-key-prompt-label"}(language->get("hostKeyPrompter", labelKey));
    }

    /**
     * @brief The key type and the fingerprint of the key the server presented.
     */
    Nui::ElementRenderer keyRows(HostKeyQuestion const& question)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;
        return fragment(
            label("keyType"),
            span{class_ = "host-key-prompt-value"}(question.keyType),
            label("fingerprint"),
            span{class_ = "host-key-prompt-fingerprint"}(question.fingerprint)
        );
    }

    Nui::ElementRenderer unknownKeyBody(HostKeyQuestion const& question)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        return fragment(
            paragraph(textAbout(question, "text")),
            paragraph(language->get("hostKeyPrompter", "advice")),
            div{class_ = "host-key-prompt-details"}(keyRows(question)),
            paragraph(language->get("hostKeyPrompter", "choices"))
        );
    }

    /**
     * @brief What a changed key differs from, by the kind of change and whose key it is: the text about it, what an
     *        attacker gains, the label and the value of what the key was compared with, and the advice.
     */
    struct ChangedKeyTexts
    {
        char const* text;
        char const* explanation;
        char const* comparedWithLabel;
        std::string comparedWith;
        char const* advice;
    };

    ChangedKeyTexts changedKeyTextsOf(HostKeyQuestion const& question)
    {
        const auto otherType = question.problem == HostKeyProblem::OtherType;
        if (question.problem == HostKeyProblem::ChangedSinceTrusted)
        {
            return {
                "trustedChangedText",
                "changedExplanation",
                "trustedKey",
                question.trustedKey,
                "trustedChangedAdvice",
            };
        }
        if (question.jumpHost)
        {
            return {
                otherType ? "jumpOtherTypeText" : "jumpChangedText",
                "jumpChangedExplanation",
                "knownHostsFile",
                question.knownHostsFile,
                "jumpChangedAdvice",
            };
        }
        return {
            otherType ? "otherTypeText" : "changedText",
            "changedExplanation",
            "knownHostsFile",
            question.knownHostsFile,
            "changedAdvice",
        };
    }

    /**
     * @brief The command that removes the old key, below the advice that says when to.
     */
    Nui::ElementRenderer removalCommandRow(HostKeyQuestion const& question)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        if (question.removalCommand.empty())
            return Nui::nil();
        return div{class_ = "host-key-prompt-details"}(
            label("removalCommand"), span{class_ = "host-key-prompt-fingerprint"}(question.removalCommand)
        );
    }

    Nui::ElementRenderer changedKeyBody(HostKeyQuestion const& question)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        const auto texts = changedKeyTextsOf(question);
        return fragment(
            paragraph(language->get("hostKeyPrompter", "changedAlarm"), "host-key-prompt-alarm"),
            paragraph(textAbout(question, texts.text)),
            paragraph(language->get("hostKeyPrompter", texts.explanation)),
            paragraph(language->get("hostKeyPrompter", "changedRefused"), "host-key-prompt-refused"),
            div{class_ = "host-key-prompt-details"}(
                keyRows(question),
                label(texts.comparedWithLabel),
                span{class_ = "host-key-prompt-value"}(texts.comparedWith)
            ),
            paragraph(language->get("hostKeyPrompter", texts.advice)),
            removalCommandRow(question)
        );
    }
}

struct HostKeyPrompter::Implementation
{
    std::unique_ptr<Snc::Dialog> dialog{};
    Nui::Observed<std::optional<HostKeyQuestion>> question{};

    Nui::ElementRenderer body();
    void open(HostKeyQuestion hostKeyQuestion);
    void askAboutUnknownKey(int promptId);
    void warnAboutChangedKey(int promptId);
};

Nui::ElementRenderer HostKeyPrompter::Implementation::body()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;

    // clang-format off
    return section{
        class_ = "host-key-prompt",
    }(
        observe(question),
        [this]() -> Nui::ElementRenderer {
            if (!question.value())
                return Nui::nil();
            auto const& shown = *question.value();
            if (shown.problem == HostKeyProblem::Unknown)
                return unknownKeyBody(shown);
            return changedKeyBody(shown);
        }
    );
    // clang-format on
}

void HostKeyPrompter::Implementation::open(HostKeyQuestion hostKeyQuestion)
{
    const auto promptId = hostKeyQuestion.promptId;
    const auto unknown = hostKeyQuestion.problem == HostKeyProblem::Unknown;
    Log::info(
        "{} the host key of {}",
        unknown ? "Asking whether to trust" : "Warning that another key is known than",
        serverOf(hostKeyQuestion)
    );
    question = std::move(hostKeyQuestion);
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (unknown)
        return askAboutUnknownKey(promptId);
    warnAboutChangedKey(promptId);
}

void HostKeyPrompter::Implementation::askAboutUnknownKey(int promptId)
{
    // Trusting saves the key, as it does in OpenSSH: a key that is not saved is asked about with every run, and a
    // change of it goes unnoticed.
    auto labels = localizedButtonLabels();
    labels.ok = language->get("hostKeyPrompter", "alwaysTrust");
    dialog->setButtonLabels(labels);
    dialog->open({
        .styleVariant = Snc::StyleVariant::Warning,
        .headerText = language->get("hostKeyPrompter", "header"),
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = Snc::Dialog::Button::Cancel,
        .onClose =
            [promptId](std::optional<Snc::Dialog::Button> button)
        {
            if (button && *button == Snc::Dialog::Button::Ok)
                return answer(promptId, trustAlways);
            answer(promptId, refuse);
        },
        .modal = true,
        .mayCloseWithoutButton = true,
    });
}

void HostKeyPrompter::Implementation::warnAboutChangedKey(int promptId)
{
    // The key is refused whatever is answered, so the warning has nothing to choose.
    auto labels = localizedButtonLabels();
    labels.cancel = language->get("hostKeyPrompter", "close");
    dialog->setButtonLabels(labels);
    dialog->open({
        .styleVariant = Snc::StyleVariant::Danger,
        .headerText = language->get("hostKeyPrompter", "changedHeader"),
        .buttons = Snc::Dialog::Button::Cancel,
        .initialFocus = Snc::Dialog::Button::Cancel,
        .onClose =
            [promptId](std::optional<Snc::Dialog::Button>)
        {
            answer(promptId, refuse);
        },
        .modal = true,
        .mayCloseWithoutButton = true,
    });
}

HostKeyPrompter::HostKeyPrompter()
    : impl_{std::make_unique<Implementation>()}
{
    impl_->dialog = std::make_unique<Snc::Dialog>("HostKeyPrompter", impl_->body());
    Nui::RpcClient::registerFunction(
        "HostKeyPrompter::prompt",
        [implementation = impl_.get()](Nui::val value)
        {
            auto hostKeyQuestion = questionOf(value);
            if (!hostKeyQuestion)
                return refuseUnread(value);
            implementation->open(*std::move(hostKeyQuestion));
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(HostKeyPrompter);

Nui::ElementRenderer HostKeyPrompter::dialog()
{
    return (*impl_->dialog)();
}
