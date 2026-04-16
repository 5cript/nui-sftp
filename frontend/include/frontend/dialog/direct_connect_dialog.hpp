#pragma once

#include <persistence/state/session_options.hpp>
#include <persistence/state_holder.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>

class DirectConnectDialog
{
  public:
    struct ConfirmResult
    {
        Persistence::SshSessionOptions sshOptions{};
    };
    struct OpenOptions
    {
        std::function<void(ConfirmResult const&)> onConfirm;
    };

    DirectConnectDialog(std::string id, Persistence::StateHolder* stateHolder);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(DirectConnectDialog);

    Nui::ElementRenderer operator()();

    void open(OpenOptions options);

  private:
    Nui::ElementRenderer dialogBody();
    void checkHostValue(std::string const& value);
    void checkPortValue(std::string const& value);
    void loadFromState();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
