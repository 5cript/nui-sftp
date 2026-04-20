#pragma once

#include <ssh/async/processing_thread.hpp>
#include <ssh/sftp_error.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/termios.hpp>
#include <ssh/channel.hpp>

#include <libssh/libsshpp.hpp>

#include <expected>
#include <map>
#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <future>

namespace SecureShell
{
    class SftpSession;

    class Session
    {
      public:
        friend class Channel;
        friend class SftpSession;
        friend class FileStream;

        Session(std::function<void()> onConnectionLoss);
        ~Session();
        Session(Session const&) = delete;
        Session& operator=(Session const&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        operator ssh::Session&()
        {
            return session_;
        }

        /**
         * @brief Starts processing the session and channels.
         *
         */
        void start();

        struct PtyCreationOptions
        {
            std::optional<std::map<std::string, std::string>> environment = std::nullopt;
            // Set locale environment variable as string when set
            std::optional<std::string> localeEnv = std::nullopt;
            std::string terminalType = "xterm-256color";
            Persistence::Termios termios;
            int columns = 80;
            int rows = 24;
            bool requestShell = true;
            bool isHiddenChannel = false;
        };

        /**
         * @brief Creates a new channel as a pty.
         *
         * @return std::expected<ChannelId, int> The channel id or an error code
         */
        std::future<std::expected<std::weak_ptr<Channel>, int>> createPtyChannel(PtyCreationOptions options);

        /**
         * @brief Create a Sftp Session object
         *
         * @return std::future<std::expected<std::weak_ptr<SftpSession>, int>>
         */
        std::future<std::expected<std::weak_ptr<SftpSession>, SftpError>> createSftpSession();

        std::string getErrorMessage();

      private:
        void channelRemoveItself(Channel* channel, bool isBackElement);
        void removeAllChannels();

        void sftpSessionRemoveItself(SftpSession* sftpSession, bool isBackElement);
        void removeAllSftpSessions();

        /**
         * @brief Shuts down the session and closes all channels.
         * The session is not usable after this.
         */
        void shutdown();

        void createHiddenChannel();

      private:
        std::function<void()> onConnectionLoss_;
        SecureShell::ProcessingThread processingThread_;
        ssh::Session session_;
        std::shared_ptr<Channel> hiddenChannel_;
        std::vector<std::shared_ptr<Channel>> channels_;
        std::vector<std::shared_ptr<SftpSession>> sftpSessions_;
    };

    using AskPassCallback = int (*)(char const* prompt, char* buf, std::size_t length, int, int, void* userdata);

    struct PasswordCacheEntry
    {
        std::optional<std::string> user;
        std::string host;
        std::optional<int> port;
        std::optional<std::string> password;
    };

    std::expected<std::unique_ptr<Session>, std::string> makeSession(
        Persistence::SshSessionOptions const& sessionOptions,
        AskPassCallback askPass,
        void* askPassUserDataKeyPhrase,
        void* askPassUserDataPassword,
        std::vector<PasswordCacheEntry>* pwCache,
        std::function<void()> onConnectionLoss
    );
}