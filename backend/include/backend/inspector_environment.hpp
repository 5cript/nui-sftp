#pragma once

/**
 * @brief Removes the environment variables that make the webview serve a remote inspector.
 *
 * Must run before the webview and its child processes are created, and while the process is still single-threaded.
 * Logs a warning for every variable that was removed.
 */
void removeRemoteInspectorEnvironment();
