#pragma once

#include <nui/backend/rpc_hub.hpp>
#include <nui/window.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

/**
 * @brief CEF behaviour of the main window the page cannot provide on its own.
 * @details Chromium hands dropped files to JavaScript without their paths. The drag handler reads them when a drag
 *          enters the window and hands them to the frontend when it claims the drop (NativeFileDrop::claim), like the
 *          WKWebView hooks on macOS. The hooks also strip the navigation entries from the context menu and keep the
 *          window on the app: a reload loses the RPC initialization, a file dropped outside a drop zone or a followed
 *          link would replace the app.
 */
class CefHooks
{
  public:
    CefHooks(Nui::Window& window, Nui::RpcHub& hub);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(CefHooks);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
