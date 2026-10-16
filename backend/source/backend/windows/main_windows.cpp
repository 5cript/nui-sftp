#include <backend/windows/main_windows.hpp>

#include <windows.h>

#include <cstdio>
#include <iostream>

Main::PlatformSpecifics::PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub)
    : enableDragDrop{window, hub}
{}

namespace
{
    /**
     * @brief A standard handle that already leads somewhere: a file or pipe it was redirected to.
     */
    bool isUsable(DWORD standardHandle)
    {
        const auto handle = GetStdHandle(standardHandle);
        return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
    }
}

void attachParentConsole()
{
    // Redirected streams keep their file or pipe, only the ones without a target go to the console.
    const bool outputUsable = isUsable(STD_OUTPUT_HANDLE);
    const bool errorUsable = isUsable(STD_ERROR_HANDLE);
    const bool inputUsable = isUsable(STD_INPUT_HANDLE);
    if (outputUsable && errorUsable && inputUsable)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE* stream = nullptr;
    if (!outputUsable)
        freopen_s(&stream, "CONOUT$", "w", stdout);
    if (!errorUsable)
        freopen_s(&stream, "CONOUT$", "w", stderr);
    if (!inputUsable)
        freopen_s(&stream, "CONIN$", "r", stdin);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}
