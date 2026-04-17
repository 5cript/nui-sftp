#include <frontend/terminal/ssh_channel.hpp>

#include <log/log.hpp>

SshChannel::SshChannel(Ids::SessionId sessionId, Ids::ChannelId channelId)
    : stdoutReceiver_{}
    , stderrReceiver_{}
    , onExitReceiver_{}
    , sshChannelId_{channelId}
    , sshSessionId_{std::move(sessionId)}
    , stdoutHandler_{}
    , stderrHandler_{}
{}

SshChannel::~SshChannel() = default;

void SshChannel::open(
    std::function<void(std::string const&)> onStdout,
    std::function<void(std::string const&)> onStderr,
    std::function<void(Ids::ChannelId const&)> onExit,
    bool fileMode
)
{
    if (!fileMode)
    {
        stdoutHandler_ = std::move(onStdout);
        stderrHandler_ = std::move(onStderr);

        stdoutReceiver_ = Nui::RpcClient::autoRegisterFunction(
            "sshTerminalStdout_" + sshChannelId_.value(),
            [this](Nui::val val)
            {
                if (val.hasOwnProperty("data"))
                {
                    const std::string data = Nui::val::global("decodeUtf8Base64")(val["data"]).as<std::string>();
                    if (stdoutHandler_)
                        stdoutHandler_(data);
                }
                else
                    Log::info("sshTerminalStdout_" + sshChannelId_.value() + " received an empty message");
            }
        );

        stderrReceiver_ = Nui::RpcClient::autoRegisterFunction(
            "sshTerminalStderr_" + sshChannelId_.value(),
            [this](Nui::val val)
            {
                if (val.hasOwnProperty("data"))
                {
                    const std::string data = Nui::val::global("decodeUtf8Base64")(val["data"]).as<std::string>();
                    if (stderrHandler_)
                        stderrHandler_(data);
                }
                else
                    Log::error("sshTerminalStderr_" + sshChannelId_.value() + " received an empty message");
            }
        );

        onExitReceiver_ = Nui::RpcClient::autoRegisterFunction(
            "sshTerminalOnExit_" + sshChannelId_.value(),
            [onExit, sshChannelId = sshChannelId_](Nui::val)
            {
                if (onExit)
                    onExit(sshChannelId);
            }
        );
    }
}
void SshChannel::write(std::string const& data)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::Channel::write", sshSessionId_.value()),
        [](Nui::val)
        {
            // TODO: handle error
        },
        sshChannelId_.value(),
        Nui::val::global("encodeUtf8Base64")(data).as<std::string>()
    );
}
void SshChannel::resize(int cols, int rows)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::Channel::ptyResize", sshSessionId_.value()),
        [](Nui::val)
        {
            // TODO: handle error
        },
        sshChannelId_.value(),
        cols,
        rows
    );
}
void SshChannel::dispose(std::function<void()> onExit)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::Channel::close", sshSessionId_.value()),
        [onExit = std::move(onExit)](Nui::val)
        {
            onExit();
        },
        sshChannelId_.value()
    );
}