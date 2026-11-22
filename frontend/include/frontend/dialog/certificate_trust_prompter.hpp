#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

/**
 * @brief Asks whether to trust the certificate of an FTP server that the backend refused, for the backend's
 *        "CertificateTrustPrompter::prompt", and answers with "CertificateTrustPrompter::promptDone": the id of the
 *        prompt and "once", "always" or "refuse". The certificate is only shown, nothing of it goes back; the backend
 *        keeps the key it trusts.
 */
class CertificateTrustPrompter
{
  public:
    CertificateTrustPrompter();
    ROAR_PIMPL_SPECIAL_FUNCTIONS(CertificateTrustPrompter);

    Nui::ElementRenderer dialog();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
