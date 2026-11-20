#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <persistence/state/session_options.hpp>

#include <functional>
#include <memory>

/**
 * @brief The engine of the remote session that @p options describes, nothing for a local session.
 *
 * @param onConnectionLoss Runs when the connection is lost after the engine opened.
 */
std::unique_ptr<TerminalEngine>
makeRemoteTerminalEngine(Persistence::SessionOptions const& options, std::function<void()> onConnectionLoss);
