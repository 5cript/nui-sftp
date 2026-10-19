#pragma once

#include <string>

/**
 * @brief Name and version of the browser engine that renders the page, read at runtime.
 *
 * For example "Chromium 154.0.8037.98 (CEF 154.0.34)", "WebKitGTK 2.50.1" or "WebView2 141.0.3537.71".
 */
std::string browserEngineDescription();
