#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/components/ui5/text.hpp>
#include <ui5/components/text_area.hpp>
#include <frontend/components/ui5/list.hpp>
#include <log/log.hpp>

#include <script-nui-components/dialog.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <utility/format_bytes.hpp>

using namespace Nui::Elements;
using namespace Nui::Attributes;
namespace Snc = ScriptNuiComponents;

struct FilePropertyDialog::Implementation
{
    std::string id;
    Nui::Observed<SharedData::DirectoryEntry> entry;
    Snc::Dialog dialog;

    Implementation(std::string id)
        : id{std::move(id)}
        , entry{}
        , dialog{
              "FilePropertyDialog_" + [](){
                static int counter;
                return std::to_string(counter++);
              }(),
              Nui::Elements::section{
                  class_ = "file-property-dialog-content",
              }(Nui::Elements::div{}(
                    Nui::Elements::span{}("Path"),
                    Snc::textInput(
                        Snc::TextInputOptions{
                            .value = observe(entry)
                                .generate(
                                    [](SharedData::DirectoryEntry const& entry)
                                    {
                                        return entry.path.generic_string();
                                    }
                                ),
                            .attributes =
                                {
                                    readOnly = true,
                                }
                        }
                    )
                ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Size"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return fmt::format(
                                          "{} ({} bytes)",
                                          Utility::formatBytes(entry.size),
                                          entry.size
                                      );
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Type"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return fileTypeToString(entry.type);
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Permissions"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return entry.lsStyleTypePermsUserGroup();
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Creation Date"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return entry.readableCreateTime();
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Last Modified"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return entry.readableMTime();
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Access Time"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return entry.readableATime();
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("User"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return fmt::format(
                                          "{} (id: {})", entry.owner, entry.uid
                                      );
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Group"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return fmt::format(
                                          "{} (id: {})", entry.group, entry.gid
                                      );
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  ),
                  Nui::Elements::div{}(
                      Nui::Elements::span{}("Access Control List"),
                      Snc::textInput(Snc::TextInputOptions{
                          .value = observe(entry)
                              .generate(
                                  [](SharedData::DirectoryEntry const& entry)
                                  {
                                      return entry.acl;
                                  }
                              ),
                          .attributes =
                              {
                                  readOnly = true,
                              }
                      })
                  )),
          }
    {}
};

FilePropertyDialog::FilePropertyDialog(std::string id)
    : impl_{std::make_unique<Implementation>(std::move(id))}
{}

void FilePropertyDialog::open(SharedData::DirectoryEntry const& entry)
{
    impl_->entry = entry;
    Nui::globalEventContext.executeActiveEventsImmediately();

    using namespace Snc;

    impl_->dialog.open({
        .styleVariant = StyleVariant::Regular,
        .initialFocus = Dialog::Button::Ok,
        .mayCloseWithoutButton = true,
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FilePropertyDialog);

Nui::ElementRenderer FilePropertyDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return impl_->dialog();
    // clang-format on
}
