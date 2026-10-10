#pragma once

#include <nui/backend/rpc_hub.hpp>
#include <nui/window.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

/**
 * @brief Native WKWebView behaviour the page cannot provide on its own.
 * @details WKWebView hands dropped files to JavaScript without their paths. The hooks read the paths
 *          from the dragging pasteboard and hand them to the frontend when it claims the drop
 *          (NativeFileDrop::claim), mirroring the WebView2 drop on Windows. They also strip the
 *          navigation entries from the context menu, like the WebKitGTK filter on Linux.
 */
class MacWebViewHooks
{
  public:
    MacWebViewHooks(Nui::Window& window, Nui::RpcHub& hub);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(MacWebViewHooks);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
