#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <log/log.hpp>

#include <script-nui-components/carousel.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <utility/format_bytes.hpp>
#include <utility/language.hpp>

namespace Snc = ScriptNuiComponents;

namespace
{
    std::string localizedFileType(SharedData::FileType type)
    {
        using enum SharedData::FileType;
        switch (type)
        {
            case Regular:
                return language->get("filePropertyDialog", "typeRegular");
            case Directory:
                return language->get("filePropertyDialog", "typeDirectory");
            case Symlink:
                return language->get("filePropertyDialog", "typeSymlink");
            case Special:
                return language->get("filePropertyDialog", "typeSpecial");
            case Socket:
                return language->get("filePropertyDialog", "typeSocket");
            case CharDevice:
                return language->get("filePropertyDialog", "typeCharDevice");
            case BlockDevice:
                return language->get("filePropertyDialog", "typeBlockDevice");
            case Fifo:
                return language->get("filePropertyDialog", "typeFifo");
            case Unknown:
            default:
                return language->get("filePropertyDialog", "typeUnknown");
        }
    }
}

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
            span{}(language->getObserved("filePropertyDialog", "path")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.path.generic_string();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "size")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fmt::format(
                        fmt::runtime(language->get("filePropertyDialog", "sizeValue")),
                        Utility::formatBytes(entry.size),
                        entry.size
                    );
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "type")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return localizedFileType(entry.type);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "permissions")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.lsStyleTypePermsUserGroup();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "creationDate")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.readableCreateTime();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "lastModified")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.readableMTime();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "accessTime")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return entry.readableATime();
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "user")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fmt::format(fmt::runtime(language->get("filePropertyDialog", "idValue")), entry.owner, entry.uid);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "group")),
            Snc::textInput(Snc::TextInputOptions{
                .value = observe(obs).generate([](SharedData::DirectoryEntry const& entry) {
                    return fmt::format(fmt::runtime(language->get("filePropertyDialog", "idValue")), entry.group, entry.gid);
                }),
                .attributes = {readOnly = true},
            })
        ),
        div{}(
            span{}(language->getObserved("filePropertyDialog", "accessControlList")),
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

    impl_->dialog.setButtonLabels(localizedButtonLabels());
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
