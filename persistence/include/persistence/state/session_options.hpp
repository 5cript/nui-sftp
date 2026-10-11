#pragma once

#include <persistence/state_core.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/sftp_options.hpp>
#include <persistence/state/queue_options.hpp>
#include <persistence/state/history_options.hpp>
#include <persistence/reference.hpp>
#include <nlohmann/json.hpp>
#include <persistence/state/terminal_options.hpp>

#include <map>
#include <optional>
#include <vector>
#include <string>
#include <filesystem>
#include <variant>

namespace Persistence
{
    enum class TerminalEngineType
    {
        shell,
        ssh
    };
    BOOST_DESCRIBE_ENUM(TerminalEngineType, shell, ssh);

    /**
     * @brief The shell a local session runs when nothing else is configured: zsh on macOS, bash elsewhere.
     */
    std::filesystem::path defaultShell();

    struct ExecutingSessionOptions : public DefaultMissingMember
    {
        bool isPty{true};
        std::filesystem::path command{defaultShell()};
        std::optional<std::vector<std::string>> arguments{std::nullopt};
        std::optional<std::map<std::string, std::string>> environment{std::nullopt};
        int exitTimeoutSeconds{5};
        bool cleanEnvironment{false};
    };
    BOOST_DESCRIBE_STRUCT(
        ExecutingSessionOptions,
        (),
        (isPty, command, arguments, environment, exitTimeoutSeconds, cleanEnvironment)
    )

    /**
     * @brief One login of a server: the user name and the key files used for it.
     */
    struct SshUser : public DefaultMissingMember
    {
        /**
         * @brief The user name to log in with. Without one, the SSH configuration or the local user name is used.
         */
        std::optional<std::string> name{std::nullopt};
        std::optional<std::filesystem::path> sshKeyPrivate{std::nullopt};
        std::optional<std::filesystem::path> sshKeyPublic{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(SshUser, (), (name, sshKeyPrivate, sshKeyPublic))

    struct SshSessionOptions : public DefaultMissingMember
    {
        std::string host{};
        std::optional<int> port{std::nullopt};

        /**
         * @brief The logins of the server. Connections log in as the first one, so the user picked for a connection
         * is moved to the front before connecting.
         */
        std::vector<SshUser> users{};
        bool openSftpByDefault{true};
        std::vector<std::string> remoteFavorites{};

        /**
         * @brief Maximum number of automatic reconnect attempts after a
         *        transport drop.  0 means "no automatic retries" (the dialog
         *        is shown for the first attempt only, then gives up on
         *        failure).  -1 means "unlimited" — the user can always Cancel.
         */
        int maxReconnectAttempts{-1};
        /**
         * @brief Cap on the exponential backoff delay between retries, in
         *        milliseconds.  Delay sequence is min(1000 << (attempt-1),
         *        maxReconnectBackoffMs).  Defaults to 16000 (16 s).
         */
        int maxReconnectBackoffMs{16000};

        // Referenceables:
        Referenceable<SshOptions> sshOptions{};
        Referenceable<SftpOptions> sftpOptions{};

        /**
         * @brief The first user, written for earlier versions that know no user list. Never read by this version.
         */
        std::optional<std::string> user{std::nullopt};
        std::optional<std::filesystem::path> sshKeyPrivate{std::nullopt};
        std::optional<std::filesystem::path> sshKeyPublic{std::nullopt};

        /**
         * @brief The login connections use, an empty one when the server has no users.
         */
        SshUser loginUser() const;

        /**
         * @brief Writes the first user into the fields earlier versions read.
         */
        void writeLegacyUser();

        /**
         * @brief Makes the user at the index the one connections log in as. Does nothing for an index out of range.
         */
        void selectUser(std::size_t index);

        /**
         * @brief Makes the user with the same name and private key the one connections log in as, or adds the user
         * in front when no such user exists anymore.
         */
        void selectUserLike(SshUser const& user);
    };
    BOOST_DESCRIBE_STRUCT(
        SshSessionOptions,
        (),
        (sshOptions,
            sftpOptions,
            host,
            port,
            users,
            openSftpByDefault,
            remoteFavorites,
            maxReconnectAttempts,
            maxReconnectBackoffMs,
            user,
            sshKeyPrivate,
            sshKeyPublic)
    )

    struct SessionOptions : public DefaultMissingMember
    {
        // Generic options:
        TerminalEngineType type{TerminalEngineType::ssh};
        std::string icon{};
        std::optional<std::string> orderBy{};
        bool startupSession{};

        // Engine:
        std::variant<std::monostate, ExecutingSessionOptions, SshSessionOptions> engine{};

        // Layout:
        std::optional<std::map<std::string, nlohmann::json>> layouts{};

        // Referenceables:
        Referenceable<TerminalOptions> terminalOptions{};
        Referenceable<Termios> termios{};
        Referenceable<QueueOptions> queueOptions{};
        Referenceable<HistoryOptions> historyOptions{};

        void variantDecide(nlohmann::json const& j)
        {
            if (j.contains("type"))
            {
                auto typeStr = j["type"].get<std::string>();
                if (typeStr == "ssh")
                    engine = SshSessionOptions{};
                else if (typeStr == "shell")
                    engine = ExecutingSessionOptions{};
                else
                    throw std::runtime_error("Unknown terminal engine type: " + typeStr);
            }
            else
            {
                throw std::runtime_error("Terminal engine type not specified");
            }
        }

        static SessionOptions create(
            std::optional<std::string> icon = std::nullopt,
            TerminalEngineType type = TerminalEngineType::ssh
        );
    };
    BOOST_DESCRIBE_STRUCT(
        SessionOptions,
        (),
        (type, icon, orderBy, startupSession, terminalOptions, termios, engine, layouts, queueOptions, historyOptions)
    )

    ExecutingSessionOptions defaultMsys2SessionOption();
    ExecutingSessionOptions defaultBashSessionOption();
    ExecutingSessionOptions defaultZshSessionOption();

    /**
     * @brief The options a new local session starts with, the zsh login shell on macOS, bash elsewhere.
     */
    ExecutingSessionOptions defaultShellSessionOption();
}