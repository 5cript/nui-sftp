#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

class NewSessionDialog
{
  public:
    enum class SessionType
    {
        ssh,
        shell
    };

    NewSessionDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(NewSessionDialog);

    Nui::ElementRenderer operator()();

    struct ConfirmResult
    {
        std::string sessionName{};
        std::string iconName{};
        SessionType sessionType{SessionType::ssh};
    };
    struct OpenOptions
    {
        std::function<void(ConfirmResult const&)> onConfirm;
        bool showIconPicker{true};
        bool showSessionTypePicker{true};
        std::string initialName{};
        std::string initialIcon{"laptop"};
        SessionType initialSessionType{SessionType::ssh};
    };
    void open(OpenOptions options);

  private:
    Nui::ElementRenderer dialogBody();
    void checkInputValue(std::string const&);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};