#include <backend/windows/main_windows.hpp>

#include <cstdio>
#include <iostream>
#include <utility>

#include <windows.h>

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

    bool reopen(char const* device, char const* mode, FILE* target)
    {
        FILE* stream = nullptr;
        return freopen_s(&stream, device, mode, target) == 0;
    }
}

ConsoleAttachment::~ConsoleAttachment()
{
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    if (previousOutputCodePage_ != 0)
        SetConsoleOutputCP(previousOutputCodePage_);
    if (previousInputCodePage_ != 0)
        SetConsoleCP(previousInputCodePage_);
}

ConsoleAttachment::ConsoleAttachment(ConsoleAttachment&& other) noexcept
    : previousOutputCodePage_{std::exchange(other.previousOutputCodePage_, 0u)}
    , previousInputCodePage_{std::exchange(other.previousInputCodePage_, 0u)}
{}

ConsoleAttachment& ConsoleAttachment::operator=(ConsoleAttachment&& other) noexcept
{
    if (this != &other)
    {
        previousOutputCodePage_ = std::exchange(other.previousOutputCodePage_, 0u);
        previousInputCodePage_ = std::exchange(other.previousInputCodePage_, 0u);
    }
    return *this;
}

ConsoleAttachment attachParentConsole()
{
    // Redirected streams keep their file or pipe, only the ones without a target go to the console.
    const bool outputUsable = isUsable(STD_OUTPUT_HANDLE);
    const bool errorUsable = isUsable(STD_ERROR_HANDLE);
    const bool inputUsable = isUsable(STD_INPUT_HANDLE);
    if (outputUsable && errorUsable && inputUsable)
        return {};
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return {};

    ConsoleAttachment attachment{};
    const bool outputReopened = !outputUsable && reopen("CONOUT$", "w", stdout);
    const bool errorReopened = !errorUsable && reopen("CONOUT$", "w", stderr);
    const bool inputReopened = !inputUsable && reopen("CONIN$", "r", stdin);
    if (outputReopened || errorReopened)
    {
        attachment.previousOutputCodePage_ = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
        std::cout.clear();
        std::cerr.clear();
    }
    if (inputReopened)
    {
        attachment.previousInputCodePage_ = GetConsoleCP();
        SetConsoleCP(CP_UTF8);
        std::cin.clear();
    }
    return attachment;
}
