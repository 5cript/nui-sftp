#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <frontend/terminal/remote_session_engine.hpp>
#include <persistence/state/session_options.hpp>
#include <ids/ids.hpp>
#include <utility/lifetime_token.hpp>

#include <roar/detail/pimpl_special_functions.hpp>
#include <nui/utility/move_detector.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief The engine of an FTP session: a backend session with file system channels and no terminals.
 */
class FtpTerminalEngine
    : public TerminalEngine
    , public RemoteSessionEngine
{
  public:
    struct Settings
    {
        /**
         * @brief The server and how to log in, with the references resolved.
         */
        Persistence::FtpSessionOptions options;
        std::function<void()> onConnectionLoss;
    };

  public:
    explicit FtpTerminalEngine(Settings settings);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FtpTerminalEngine);

    /**
     * @brief Connects to the server and logs in, see TerminalEngine::open. The backend logs its connect errors with
     *        @p failureToast too.
     */
    void open(std::function<void(bool, std::string const&)> onOpen, Log::Toast failureToast) override;

    std::string engineName() const override
    {
        return "ftp";
    }

    void dispose(std::function<void()> onDisposeComplete) override;

    /**
     * @brief Refuses, an FTP session has no terminals.
     */
    void createChannel(
        ChannelCreationOptions const& options,
        std::function<void(std::string const&)> handler,
        std::function<void(std::string const&)> errorHandler,
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss
    ) override;

    /**
     * @brief Closes a file system channel, the only channels this engine has.
     */
    void closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose = []() {}) override;

    /**
     * @brief Nothing, an FTP session has no terminal channels.
     */
    ChannelInterface* channel(Ids::ChannelId const& channelId) override;

    RemoteSessionEngine* remoteSession() override
    {
        return this;
    }

    Ids::SessionId sessionId() const override;
    void createFileChannel(
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated
    ) override;
    void closeFileChannel(Ids::ChannelId const& channelId, std::function<void()> onClosed) override;
    void setOnConnectionLoss(std::function<void()> onConnectionLoss) override;

  private:
    void onSuccessfulOpen();

    /**
     * @brief Ends the session once. A lost connection only tells the owner, the backend removed the session already.
     */
    void disconnect(std::function<void()> onDisconnect, bool byLossOfConnection);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
    /**
     * @brief Drops the replies of the backend when this engine is gone.
     */
    Utility::LifetimeToken lifetime_{};
};
