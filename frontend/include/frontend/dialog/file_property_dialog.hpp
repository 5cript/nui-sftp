#pragma once

#include <shared_data/directory_entry.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

class FilePropertyDialog
{
  public:
    FilePropertyDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FilePropertyDialog);

    Nui::ElementRenderer operator()();

    void open(SharedData::DirectoryEntry const& entry);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};