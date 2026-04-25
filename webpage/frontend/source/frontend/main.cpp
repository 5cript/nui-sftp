#include <frontend/page_frame.hpp>

#include <nui/core.hpp>
#include <nui/frontend/bindings.hpp>
#include <nui/frontend/dom/dom.hpp>

#include <memory>

namespace
{
    std::unique_ptr<NuiSftpPage::PageFrame> pageFrame;
    std::unique_ptr<Nui::Dom::Dom> dom;
}

extern "C" void frontendMain()
{
    pageFrame = std::make_unique<NuiSftpPage::PageFrame>();
    dom = std::make_unique<Nui::Dom::Dom>();
    dom->setBody(pageFrame->render());
}

EMSCRIPTEN_BINDINGS(nui_sftp_webpage)
{
    emscripten::function("main", &frontendMain);
}
