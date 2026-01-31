#pragma once

#include <nui/window.hpp>
#include <nui/backend/rpc_hub.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

// https://github.com/MicrosoftEdge/WebView2Samples/blob/main/SampleApps/WebView2APISample/ScenarioDragDrop.h

// https://github.com/MicrosoftEdge/WebView2Samples/blob/main/SampleApps/WebView2APISample/assets/ScenarioDragDrop.html

// https://github.com/MicrosoftEdge/WebView2Samples/blob/main/SampleApps/WebView2APISample/ScenarioDragDrop.cpp

// WIL/com_ptr: https://github.com/microsoft/wil/blob/0bd7bf251bcb43b7d235d386a8ba70a8aa2f2560/include/wil/com.h#L193

class EnableWindowsDragDrop
{
  public:
    EnableWindowsDragDrop(Nui::Window& wnd, Nui::RpcHub& hub);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(EnableWindowsDragDrop);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};