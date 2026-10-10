#pragma once

#include <string>
#include <vector>

namespace PTY
{
    struct TerminalProcess
    {
        int pid;
        std::string cmdline;
    };

    /**
     * @brief Lists the processes whose standard input is the given terminal, sorted by pid.
     * @param terminalName Device path of the terminal, as returned by ptsname (e.g. /dev/pts/3 or /dev/ttys003).
     */
    std::vector<TerminalProcess> listProcessesOnTerminal(std::string const& terminalName);
}
