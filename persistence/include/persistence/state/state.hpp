#pragma once

#include <log/level.hpp>
#include <persistence/state_core.hpp>

#include <persistence/state/session_options.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/ui_options.hpp>
#include <persistence/state/queue_options.hpp>
#include <persistence/state/history_options.hpp>
#include <persistence/state/file_tracking_options.hpp>
#include <persistence/state/local_filesystem_options.hpp>
#include <persistence/state/localization_options.hpp>
#include <persistence/state/log_options.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace Persistence
{
    /**
     * @brief The inheritable option groups a session is based on, by their names.
     */
    struct InheritableReferences : public DefaultMissingMember
    {
        std::string terminalOptions{"default"};
        std::string termios{"default"};
        std::string queueOptions{"default"};
        std::string historyOptions{"default"};
        std::string sshOptions{"default"};
        std::string sftpOptions{"default"};

        /**
         * @brief Points the inheritable options of the session at these groups.
         */
        void applyTo(SessionOptions& session) const;
    };
    BOOST_DESCRIBE_STRUCT(
        InheritableReferences,
        (),
        (terminalOptions, termios, queueOptions, historyOptions, sshOptions, sftpOptions)
    )

    struct State : public DefaultMissingMember
    {
        std::map<std::string, TerminalOptions> terminalOptions{};
        std::map<std::string, Termios> termios{};
        std::map<std::string, SshOptions> sshOptions{};
        std::map<std::string, SftpOptions> sftpOptions{};
        std::map<std::string, SessionOptions> sessions{};
        std::map<std::string, QueueOptions> queueOptions{};
        std::map<std::string, HistoryOptions> historyOptions{};
        FileTrackingOptions fileTrackingOptions{};
        LocalFilesystemOptions localFilesystemOptions{};
        UiOptions uiOptions{};
        LogOptions logOptions{};
        LocalizationOptions localizationOptions{};
        std::optional<SshSessionOptions> lastDirectConnect{};
        std::optional<InheritableReferences> lastDirectConnectInheritables{};

        State fullyResolve() const;

        /**
         * @brief Fills what the session leaves unset from the inheritable options it references.
         */
        void resolveReferencesOf(SessionOptions& session) const;

        /**
         * @brief Writes the first user of every SSH server into the fields earlier versions read, so switching back
         * to one keeps the servers usable.
         */
        void writeLegacyUsers();
    };

    BOOST_DESCRIBE_STRUCT(
        State,
        (),
        (terminalOptions,
            sessions,
            termios,
            sshOptions,
            sftpOptions,
            uiOptions,
            logOptions,
            localizationOptions,
            queueOptions,
            historyOptions,
            fileTrackingOptions,
            localFilesystemOptions,
            lastDirectConnect,
            lastDirectConnectInheritables,
            syncBaselines)
    )
}