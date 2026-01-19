#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/session_icon_options.hpp>
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
    std::string icon{"laptop"};

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
        impl_->nameValid = true;
        Nui::globalEventContext.executeActiveEventsImmediately();
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

    auto makeOption = [](std::string_view icon)
    {
        return ui5::option{"icon"_prop = icon, "data-icon"_attr = icon}();
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
                [&makeOption]{
                    std::vector<Nui::ElementRenderer> options;
                    for (auto icon : sessionIconOptions)
                        options.push_back(makeOption(icon));
                    return options;
                }()
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
