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

    /**
     * @brief Shows @p entry found at @p path. A symlink's target is shown at @p targetPath, which may be empty.
     */
    void open(SharedData::DirectoryEntry const& entry, std::string path, std::string targetPath = {});

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};