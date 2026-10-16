#pragma once

#include <backend/main.hpp>
#include <backend/windows/windows_drag_drop.hpp>

struct Main::PlatformSpecifics
{
    PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub);

    EnableWindowsDragDrop enableDragDrop;
};

/**
 * @brief The console the standard streams were pointed at. It writes and reads UTF-8 while this lives and gets its
 *        previous code pages back when this is destroyed, since the console belongs to the parent as well.
 */
class ConsoleAttachment
{
  public:
    ConsoleAttachment() = default;
    ~ConsoleAttachment();
    ConsoleAttachment(ConsoleAttachment&& other) noexcept;
    ConsoleAttachment& operator=(ConsoleAttachment&& other) noexcept;
    ConsoleAttachment(ConsoleAttachment const&) = delete;
    ConsoleAttachment& operator=(ConsoleAttachment const&) = delete;

  private:
    friend ConsoleAttachment attachParentConsole();

    /**
     * @brief The output code page to restore, 0 when it was not changed.
     */
    unsigned int previousOutputCodePage_{0};
    /**
     * @brief The input code page to restore, 0 when it was not changed.
     */
    unsigned int previousInputCodePage_{0};
};

/**
 * @brief Attaches to the console of the process that started this one, if there is one, and points the standard
 *        streams without a file or pipe at it. The program is a GUI program, without this its output goes nowhere.
 */
ConsoleAttachment attachParentConsole();