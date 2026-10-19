#pragma once

#include <log/level.hpp>
#include <persistence/state_core.hpp>

#include <persistence/state/session_options.hpp>
#include <persistence/state/session_layouts.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/ui_options.hpp>
#include <persistence/state/queue_options.hpp>
#include <persistence/state/history_options.hpp>
#include <persistence/state/file_tracking_options.hpp>
#include <persistence/state/local_filesystem_options.hpp>
#include <persistence/state/localization_options.hpp>
#include <persistence/state/log_options.hpp>

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
        /**
         * @brief The names of the one-time migrations already applied, so a value set afterwards is kept.
         */
        std::optional<std::vector<std::string>> appliedMigrations{};
        /**
         * @brief The layouts shared by the sessions of a type. Missing until gathered from the servers.
         */
        std::optional<SessionLayouts> sessionLayouts{};

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

        /**
         * @brief Moves the terminal renderer from DOM to WebGL once. Earlier Linux versions had to default to DOM.
         *
         * @return Whether the migration ran and the state has to be saved.
         */
        bool migrateTerminalRendererFromDom();

        /**
         * @brief Gathers the layouts earlier versions saved per server into the layouts of their type, if not done
         * yet. A name used with a different layout by another server of the type gets the server name appended.
         * The layouts of the servers stay for earlier versions.
         *
         * @return Whether the layouts were gathered and the state has to be saved.
         */
        bool gatherSessionLayouts();
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
            appliedMigrations,
            sessionLayouts)
    )
}