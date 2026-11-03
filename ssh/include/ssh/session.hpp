#pragma once

#include <remote_io/async/processing_thread.hpp>
#include <remote_io/connection.hpp>
#include <remote_io/remote_error.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/termios.hpp>
#include <ssh/channel.hpp>

#include <libssh/libsshpp.hpp>

#include <chrono>
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

    class Session : public RemoteIo::IConnection
    {
      public:
        friend class Channel;
        friend class SftpSession;
        friend class FileStream;

        Session(std::function<void()> onConnectionLoss);
        ~Session() override;
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
        void start() override;

        /**
         * @brief Sets how long every libssh call waits for the server, before start.
         * @return What libssh returned, SSH_OK when it took the timeout.
         */
        int setCallTimeout(std::chrono::microseconds timeout);

        /**
         * @brief How long a libssh call waits for the server, see setCallTimeout. The maximum when it was never set,
         *        libssh then waits forever.
         */
        std::chrono::microseconds callTimeout() const
        {
            return callTimeout_;
        }

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
        std::future<std::expected<std::weak_ptr<SftpSession>, RemoteIo::RemoteError>> createSftpSession();

        /**
         * @brief An SFTP session as the protocol neutral file system, see createSftpSession.
         */
        std::future<std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteIo::RemoteError>>
        createFileSystem() override;

        std::string getErrorMessage();

      private:
        std::expected<std::weak_ptr<SftpSession>, RemoteIo::RemoteError> createSftpSessionInThread();

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
        std::chrono::microseconds callTimeout_{std::chrono::microseconds::max()};
        RemoteIo::ProcessingThread processingThread_;
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

    /**
     * @brief How long every libssh call after the connect waits for the server: the timeout @p options set, 30 s when
     *        they set none, since libssh then waits forever and file system calls rely on it to end. A ConnectTimeout
     *        of the ssh config bounds the connect only.
     */
    std::chrono::microseconds callTimeoutOf(Persistence::SshOptions const& options);

    std::expected<std::unique_ptr<Session>, std::string> makeSession(
        Persistence::SshSessionOptions const& sessionOptions,
        AskPassCallback askPass,
        void* askPassUserDataKeyPhrase,
        void* askPassUserDataPassword,
        std::vector<PasswordCacheEntry>* pwCache,
        std::function<void()> onConnectionLoss
    );
}