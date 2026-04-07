#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

class NewSessionDialog
{
  public:
    NewSessionDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(NewSessionDialog);

    Nui::ElementRenderer operator()();

    struct ConfirmResult
    {
        std::string sessionName{};
        std::string iconName{};
    };
    struct OpenOptions
    {
        std::function<void(ConfirmResult const&)> onConfirm;
        bool showIconPicker{true};
        std::string initialName{};
        std::string initialIcon{"laptop"};
    };
    void open(OpenOptions options);

  private:
    Nui::ElementRenderer dialogBody();
    void checkInputValue(std::string const&);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};