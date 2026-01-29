#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/components/ui5/text.hpp>
#include <ui5/components/text_area.hpp>
#include <frontend/components/ui5/list.hpp>
#include <log/log.hpp>

#include <ui5/components/dialog.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/label.hpp>
#include <ui5/components/input.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <utility/format_bytes.hpp>

struct FilePropertyDialog::Implementation
{
    std::string id;
    std::weak_ptr<Nui::Dom::BasicElement> dialog;
    Nui::Observed<SharedData::DirectoryEntry> entry;

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , entry{}
    {}
};

FilePropertyDialog::FilePropertyDialog(std::string id)
    : impl_{std::make_unique<Implementation>(std::move(id))}
{}

void FilePropertyDialog::open(SharedData::DirectoryEntry const& entry)
{
    impl_->entry = entry;
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (auto diag = impl_->dialog.lock(); diag)
    {
        diag->val().set("open", true);
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FilePropertyDialog);

void FilePropertyDialog::close()
{
    if (auto diag = impl_->dialog.lock(); diag)
    {
        Log::info("Closing dialog");
        diag->val().set("open", false);
    }
}

Nui::ElementRenderer FilePropertyDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return ui5::dialog{
        id = "FilePropertyDialog_" + impl_->id,
        "state"_prop = "Information",
        "headerText"_prop = observe(impl_->entry).generate([this](){
            return "Properties of " + impl_->entry.value().path.filename().generic_string();
        }),
        reference = impl_->dialog,
    }(
        section{
            class_ = "file-property-dialog-content",
        }(
            div{}(
                ui5::label{}("Path"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return impl_->entry.value().path.generic_string();
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Size"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return fmt::format("{} ({} bytes)", Utility::formatBytes(impl_->entry.value().size), impl_->entry.value().size);
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Type"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return fileTypeToString(impl_->entry.value().type);
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Permissions"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return impl_->entry.value().lsStyleTypePermsUserGroup();
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Creation Date"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return impl_->entry.value().readableCreateTime();
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Last Modified"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return impl_->entry.value().readableMTime();
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Access Time"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return impl_->entry.value().readableATime();
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("User"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return fmt::format("{} (id: {})", impl_->entry.value().owner, impl_->entry.value().uid);
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Group"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return fmt::format("{} (id: {})", impl_->entry.value().group, impl_->entry.value().gid);
                    }),
                    "readonly"_prop = true,
                }()
            ),
            div{}(
                ui5::label{}("Access Control List"),
                ui5::input{
                    "value"_prop = observe(impl_->entry).generate([this]() {
                        return impl_->entry.value().acl;
                    }),
                    "readonly"_prop = true,
                }()
            )
        ),
        div{
            "slot"_attr = "footer",
            style="display: flex; justify-content: flex-end; width: 100%; align-items: center; gap: 10px; padding: 10px;"
        }(
            div{style = "flex: 1;"}(),
            ui5::button{
                "click"_event = [this](Nui::val) {
                    close();
                }
            }("Ok")
        )
    );
    // clang-format on
}
