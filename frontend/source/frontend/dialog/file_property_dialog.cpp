#include <frontend/dialog/file_property_dialog.hpp>
#include <log/log.hpp>

#include <script-nui-components/carousel.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <utility/format_bytes.hpp>

namespace Snc = ScriptNuiComponents;

struct FilePropertyDialog::Implementation
{
    std::string id;
    Nui::Observed<SharedData::DirectoryEntry> entry;
    Nui::Observed<SharedData::DirectoryEntry> targetEntry;
    std::shared_ptr<Nui::Observed<int>> carouselPage;
    std::unique_ptr<Snc::Carousel> carousel;
    Snc::Dialog dialog;

    Nui::ElementRenderer renderEntrySection(Nui::Observed<SharedData::DirectoryEntry>& obs);

    Implementation(std::string ident)
        : id{std::move(ident)}
        , entry{}
        , targetEntry{}
        , carouselPage{std::make_shared<Nui::Observed<int>>(0)}
        , carousel{std::make_unique<Snc::Carousel>(
              carouselPage,
              [this](int page) -> Nui::ElementRenderer
              {
                  if (page == 0)
                      return renderEntrySection(entry);
                  return renderEntrySection(targetEntry);
              },
              1
          )}
        , dialog{
              "FilePropertyDialog_" +
                  []()
              {
                  static int counter;
                  return std::to_string(counter++);
              }(),
              (*carousel)()}
    {}
};

Nui::ElementRenderer FilePropertyDialog::Implementation::renderEntrySection(
    Nui::Observed<SharedData::DirectoryEntry>& obs)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return section{
        class_ = "file-property-dialog-content",
    }(
        div{}(
            span{}("Path"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.path.generic_string();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Size"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fmt::format("{} ({} bytes)", Utility::formatBytes(entry.size), entry.size);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Type"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fileTypeToString(entry.type);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Permissions"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.lsStyleTypePermsUserGroup();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Creation Date"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.readableCreateTime();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Last Modified"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.readableMTime();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Access Time"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.readableATime();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("User"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fmt::format("{} (id: {})", entry.owner, entry.uid);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Group"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fmt::format("{} (id: {})", entry.group, entry.gid);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}("Access Control List"),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.acl;
                }),
                .attributes = {readOnly = true},
            })
        )
    );
    // clang-format on
}

FilePropertyDialog::FilePropertyDialog(std::string id)
    : impl_{std::make_unique<Implementation>(std::move(id))}
{}

void FilePropertyDialog::open(SharedData::DirectoryEntry const& entry)
{
    impl_->entry = entry;
    *impl_->carouselPage = 0;
    if (entry.resolvedTarget)
    {
        impl_->targetEntry = *entry.resolvedTarget;
        impl_->carousel->setItemCount(2);
    }
    else
    {
        impl_->carousel->setItemCount(1);
    }
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog.open({
        .styleVariant = Snc::StyleVariant::Regular,
        .initialFocus = Snc::Dialog::Button::Ok,
        .mayCloseWithoutButton = true,
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FilePropertyDialog);

Nui::ElementRenderer FilePropertyDialog::operator()()
{
    return impl_->dialog();
}
