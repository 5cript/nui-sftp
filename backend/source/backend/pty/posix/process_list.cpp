#include <backend/pty/posix/process_list.hpp>

#ifdef __APPLE__
#    include <libproc.h>
#    include <sys/stat.h>
#    include <sys/sysctl.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace PTY
{
    namespace
    {
#ifdef __APPLE__
        /**
         * @brief Reads the first argument of a process from KERN_PROCARGS2, matching what /proc/<pid>/cmdline
         *        yields up to its first NUL on Linux.
         */
        std::string firstArgument(pid_t pid)
        {
            int argumentMaximum = 0;
            std::size_t size = sizeof(argumentMaximum);
            int maximumQuery[] = {CTL_KERN, KERN_ARGMAX};
            if (::sysctl(maximumQuery, 2, &argumentMaximum, &size, nullptr, 0) != 0 || argumentMaximum <= 0)
                return {};

            std::string buffer(static_cast<std::size_t>(argumentMaximum), '\0');
            size = buffer.size();
            int argumentsQuery[] = {CTL_KERN, KERN_PROCARGS2, pid};
            if (::sysctl(argumentsQuery, 3, buffer.data(), &size, nullptr, 0) != 0 || size <= sizeof(int))
                return {};

            // Layout: int argc, executable path, NUL padding, argv[0], ...
            auto position = buffer.begin() + sizeof(int);
            const auto end = buffer.begin() + static_cast<std::ptrdiff_t>(size);
            position = std::find(position, end, '\0');
            position = std::find_if(
                position,
                end,
                [](char character)
                {
                    return character != '\0';
                }
            );
            return std::string{position, std::find(position, end, '\0')};
        }

        std::vector<TerminalProcess> listProcesses(std::string const& terminalName)
        {
            struct stat terminalStatus{};
            if (::stat(terminalName.c_str(), &terminalStatus) != 0)
                return {};

            const int count = ::proc_listallpids(nullptr, 0);
            if (count <= 0)
                return {};
            std::vector<pid_t> pids(static_cast<std::size_t>(count) * 2u);
            const int filled = ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
            if (filled <= 0)
                return {};
            pids.resize(static_cast<std::size_t>(filled));

            std::vector<TerminalProcess> processes;
            for (const pid_t pid : pids)
            {
                struct proc_bsdinfo info{};
                if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
                    continue;
                if (info.e_tdev != static_cast<std::uint32_t>(terminalStatus.st_rdev))
                    continue;
                auto cmdline = firstArgument(pid);
                // The arguments are unreadable while a process execs, the short name always is.
                if (cmdline.empty())
                    cmdline = info.pbi_name[0] != '\0' ? info.pbi_name : info.pbi_comm;
                processes.push_back(TerminalProcess{.pid = pid, .cmdline = std::move(cmdline)});
            }
            return processes;
        }
#else
        std::vector<TerminalProcess> listProcesses(std::string const& terminalName)
        {
            std::vector<TerminalProcess> processes;
            std::error_code iterationError;
            for (auto const& entry : std::filesystem::directory_iterator("/proc", iterationError))
            {
                const auto id = entry.path().filename().string();
                if (id.empty() ||
                    !std::all_of(
                        id.begin(),
                        id.end(),
                        [](unsigned char character)
                        {
                            return std::isdigit(character);
                        }
                    ))
                {
                    continue;
                }

                // Unreadable entries (other users' processes) fail here and are skipped.
                std::error_code linkError;
                const auto target = std::filesystem::read_symlink(entry.path() / "fd" / "0", linkError);
                if (linkError || target.string() != terminalName)
                    continue;

                std::ifstream cmdlineFile{entry.path() / "cmdline"};
                if (!cmdlineFile)
                    continue;
                std::string cmdline;
                std::getline(cmdlineFile, cmdline, '\0');
                processes.push_back(TerminalProcess{.pid = std::stoi(id), .cmdline = std::move(cmdline)});
            }
            return processes;
        }
#endif
    }

    std::vector<TerminalProcess> listProcessesOnTerminal(std::string const& terminalName)
    {
        auto processes = listProcesses(terminalName);
        std::sort(
            processes.begin(),
            processes.end(),
            [](TerminalProcess const& lhs, TerminalProcess const& rhs)
            {
                return lhs.pid < rhs.pid;
            }
        );
        return processes;
    }
}
