#include <frontend/terminal/remote_engine_factory.hpp>
#include <frontend/terminal/ftp_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>

#include <utility>
#include <variant>

std::unique_ptr<TerminalEngine>
makeRemoteTerminalEngine(Persistence::SessionOptions const& options, std::function<void()> onConnectionLoss)
{
    if (std::holds_alternative<Persistence::SshSessionOptions>(options.engine))
    {
        return std::make_unique<SshTerminalEngine>(SshTerminalEngine::Settings{
            .sessionOptions = options,
            .onConnectionLoss = std::move(onConnectionLoss),
        });
    }
    if (auto const* ftp = std::get_if<Persistence::FtpSessionOptions>(&options.engine))
    {
        return std::make_unique<FtpTerminalEngine>(FtpTerminalEngine::Settings{
            .options = *ftp,
            .onConnectionLoss = std::move(onConnectionLoss),
        });
    }
    return nullptr;
}
