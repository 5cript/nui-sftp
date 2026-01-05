#include <frontend/dialog/new_session_dialog.hpp>
#include <log/log.hpp>

#include <nui/rpc.hpp>
#include <ui5/components/dialog.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/input.hpp>
#include <ui5/components/label.hpp>
#include <ui5/components/select.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <regex>

struct NewSessionDialog::Implementation
{
    static constexpr std::string_view defaultName = "";

    std::string id;
    std::weak_ptr<Nui::Dom::BasicElement> dialog;
    std::weak_ptr<Nui::Dom::BasicElement> input;
    std::function<void(NewSessionDialog::ConfirmResult const&)> onConfirm;
    Nui::Observed<bool> nameValid{false};
    std::string icon{};

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , input{}
        , onConfirm{}
    {}
};

NewSessionDialog::NewSessionDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{}

void NewSessionDialog::open(std::function<void(ConfirmResult const&)> onConfirm)
{
    impl_->onConfirm = onConfirm;

    if (auto input = impl_->input.lock())
    {
        input->val().set("value", std::string{Implementation::defaultName});
    }

    if (auto diag = impl_->dialog.lock(); diag)
    {
        diag->val().set("open", true);
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(NewSessionDialog);

void NewSessionDialog::closeDialog(std::optional<ConfirmResult> const& result)
{
    if (auto diag = impl_->dialog.lock(); diag)
        diag->val().set("open", false);

    if (impl_->onConfirm && result && impl_->nameValid.value())
        impl_->onConfirm(*result);
}

void NewSessionDialog::confirm()
{
    if (auto input = impl_->input.lock())
    {
        closeDialog(
            ConfirmResult{
                .sessionName = input->val()["value"].as<std::string>(),
                .iconName = impl_->icon,
            }
        );
        input->val().set("value", std::string{Implementation::defaultName});
    }
    else
        closeDialog(std::nullopt);
}

void NewSessionDialog::checkInputValue()
{
    if (auto input = impl_->input.lock())
    {
        const auto value = input->val()["value"].as<std::string>();
        std::regex validator(R"(^[^\\"]{1,5000}$)");
        impl_->nameValid = std::regex_match(value, validator);
    }
}

Nui::ElementRenderer NewSessionDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    auto makeOption = [](Nui::StringLiteral lit)
    {
        return ui5::option{"icon"_prop = lit.c_str, "data-icon"_attr = lit.c_str}();
    };

    // clang-format off
    return ui5::dialog{
        id = "InputDialog_" + impl_->id,
        "headerText"_prop = "Create New Session",
        reference = impl_->dialog,
    }(
        section{class_ = "new-session-section"}(
            ui5::label{id = "NewSessionDialogLabel_" + impl_->id, "for"_prop = "NewSessionDialogInput_" + impl_->id}(
                "Session name: "
            ),
            ui5::input{
                id = "NewSessionDialogInput_" + impl_->id,
                "type"_prop = "Text",
                "valueState"_prop = observe(impl_->nameValid).generate([](bool valid){
                    if (valid)
                        return "None";
                    return "Negative";
                }),
                reference = impl_->input,
                "keyup"_event = [this](Nui::val event){
                    if (event["key"].as<std::string>() == "Enter")
                        return confirm();
                    checkInputValue();
                }
            }(
                div{
                    "slot"_attr = "valueStateMessage",
                    "slot"_prop = "valueStateMessage"
                }("Session name must be 1-5000 characters long and cannot contain \\ or \" characters.")
            ),
            ui5::label{id = "NewSessionDialogLabel2_" + impl_->id, "for"_prop = "NewSessionDialogInput2_" + impl_->id}(
                "Icon: "
            ),
            ui5::select{
                id = "NewSessionDialogInput2_" + impl_->id,
                "type"_prop = "Text",
                "change"_event = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    Nui::WebApi::Console::log(event);
                    impl_->icon = event["detail"]["selectedOption"]["dataset"]["icon"].as<std::string>();
                }
            }(
                makeOption("laptop"),
                makeOption("ipad"),
                makeOption("iphone"),
                makeOption("account"),
                makeOption("accessibility"),
                makeOption("area-chart"),
                makeOption("favorite"),
                makeOption("fax-machine"),
                makeOption("flag"),
                makeOption("family-care"),
                makeOption("home"),
                makeOption("home-share"),
                makeOption("heart"),
                makeOption("heart-2"),
                makeOption("key"),
                makeOption("feed"),
                makeOption("it-instance"),
                makeOption("it-system"),
                makeOption("it-host"),
                makeOption("lab"),
                makeOption("machine"),
                makeOption("meal"),
                makeOption("physical-activity"),
                makeOption("primary-key"),
                makeOption("shipping-status"),
                makeOption("shield"),
                makeOption("study-leave"),
                makeOption("subway-train"),
                makeOption("syringe"),
                makeOption("tag"),
                makeOption("web-cam"),
                makeOption("sound-loud"),
                makeOption("simple-payment"),
                makeOption("print"),
                makeOption("nutrition-activity"),
                makeOption("lightbulb")
            )
        ),
        div{
            "slot"_attr = "footer",
            style="display: flex; justify-content: flex-end; width: 100%; align-items: center; gap: 10px"
        }(
            div{style = "flex: 1;"}(),
            ui5::button{
                "design"_prop = "Emphasized",
                "click"_event = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    closeDialog(std::nullopt);
                }
            }("Cancel"),
            ui5::button{
                "design"_prop = "Emphasized",
                "click"_event = [this](Nui::val){
                    confirm();
                }
            }("Ok")
        )
    );
    // clang-format on
}
