#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

class InputDialog
{
  public:
    InputDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(InputDialog);

    Nui::ElementRenderer operator()();

    struct OpenOptions
    {
        std::string whatFor{};
        std::string prompt{};
        std::string headerText{};
        std::string initialValue{};
        bool isPassword{false};
        std::function<void(std::optional<std::string> const&)> onConfirm;
    };
    void open(OpenOptions const& options);

  private:
    Nui::ElementRenderer dialogBody();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};