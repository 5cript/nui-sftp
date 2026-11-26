#pragma once

#include <ssh/host_key.hpp>

#include <functional>

/**
 * @brief Asks the user whether to trust the host key of an SSH server, see SessionManager::setHostKeyProvider().
 */
class HostKeyProvider
{
  public:
    HostKeyProvider() = default;
    virtual ~HostKeyProvider() = default;
    HostKeyProvider(HostKeyProvider const&) = delete;
    HostKeyProvider& operator=(HostKeyProvider const&) = delete;
    HostKeyProvider(HostKeyProvider&&) = delete;
    HostKeyProvider& operator=(HostKeyProvider&&) = delete;

    /**
     * @param onAnswer Called once, from any thread, with what the user decided.
     */
    virtual void askAboutHostKey(
        SecureShell::HostKeyQuestion const& question,
        std::function<void(SecureShell::HostKeyAnswer)> onAnswer
    ) = 0;
};
