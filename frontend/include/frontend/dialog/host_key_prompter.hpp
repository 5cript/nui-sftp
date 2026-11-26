#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

/**
 * @brief Asks whether to trust the host key of an SSH server that no known hosts file holds, for the backend's
 *        "HostKeyPrompter::prompt", and answers with "HostKeyPrompter::promptDone": the id of the prompt and "once",
 *        "always" or "refuse". Trusting once is confirmed in a second step. The key is only shown, nothing of it goes
 *        back; the backend trusts the key the server presented. A host key that differs from the known one, or from
 *        the one trusted without saving, is shown as a warning without a choice, the backend has refused it.
 */
class HostKeyPrompter
{
  public:
    HostKeyPrompter();
    ROAR_PIMPL_SPECIAL_FUNCTIONS(HostKeyPrompter);

    Nui::ElementRenderer dialog();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
