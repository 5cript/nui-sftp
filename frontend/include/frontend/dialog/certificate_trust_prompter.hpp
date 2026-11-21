#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

namespace Persistence
{
    class StateHolder;
}

/**
 * @brief Asks whether to trust the certificate of an FTP server that no authority vouched for, for the backend's
 *        "CertificateTrustPrompter::prompt", and answers with "CertificateTrustPrompter::promptDone". Always trust
 *        also pins the public key in every saved FTP session of that server with TLS, so later connects and
 *        reconnects trust it.
 */
class CertificateTrustPrompter
{
  public:
    explicit CertificateTrustPrompter(Persistence::StateHolder& stateHolder);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(CertificateTrustPrompter);

    Nui::ElementRenderer dialog();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
