#pragma once

#include <persistence/state/termios.hpp>
#include <frontend/terminal/terminal_engine.hpp>
#include <roar/detail/pimpl_special_functions.hpp>
#include <persistence/state/session_options.hpp>
#include <nui/utility/move_detector.hpp>

#include <memory>
#include <string>

class ExecutingTerminalEngine : public SingleChannelTerminalEngine
{
  public:
    struct Settings
    {
        Persistence::ExecutingSessionOptions engineOptions;
        Persistence::Termios termios;
        std::function<void(std::string)> onProcessChange;
    };

  public:
    ExecutingTerminalEngine(Settings settings);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ExecutingTerminalEngine);

    void open(std::function<void(bool, std::string const&)> onOpen) override;
    void dispose(std::function<void()> onDisposeComplete) override;
    void write(std::string const& data) override;
    void resize(int cols, int rows) override;
    std::string id() const;
    std::string engineName() const override
    {
        return "local";
    }

    void setStdoutHandler(std::function<void(std::string const&)> handler) override;
    void setStderrHandler(std::function<void(std::string const&)> handler) override;

  private:
    void updatePtyProcs();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};