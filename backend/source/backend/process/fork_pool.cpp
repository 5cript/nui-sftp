#include <backend/process/fork_pool.hpp>

#include <backend/process/json_process_io.hpp>
#include <nui/backend/filesystem/special_paths.hpp>
#include <persistence/state/termios.hpp>
#include <roar/utility/base64.hpp>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <pty.h>
#include <unistd.h>
#include <utmp.h>

namespace
{
    // =========================================================================
    // Worker-side — runs entirely in the child process.
    // Single-threaded, driven by epoll.  No boost, no threads, no mutexes.
    // =========================================================================

    struct WProc
    {
        pid_t pid;
        int ptyMaster; // -1 once closed
        std::string id;
    };

    struct WorkerState
    {
        int epollFd{-1};
        int readFd{-1};
        int writeFd{-1};
        int sigFd{-1};
        FdJsonIo io;
        std::unordered_map<std::string, WProc> procs; // id  -> WProc
        std::unordered_map<int, std::string> fdToId; // pty master fd -> id
        bool running{true};

        WorkerState(int readFd_, int writeFd_)
            : readFd{readFd_}
            , writeFd{writeFd_}
            , io{readFd_, writeFd_}
        {}

        void addToEpoll(int fd, std::uint32_t events) const
        {
            epoll_event ev{};
            ev.events = events;
            ev.data.fd = fd;
            if (::epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == -1)
                spdlog::error("[worker] epoll_ctl ADD fd={}: {}", fd, std::strerror(errno));
        }

        void removeFromEpoll(int fd) const
        {
            if (::epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr) == -1)
                spdlog::error("[worker] epoll_ctl DEL fd={}: {}", fd, std::strerror(errno));
        }

        void sendJson(nlohmann::json const& msg)
        {
            if (!io.writeJson(msg))
                spdlog::error("[worker] writeJson failed");
        }

        // --- PTY cleanup (shared by EIO path and signal path) -----------------

        void closePtyMaster(WProc& proc)
        {
            if (proc.ptyMaster < 0)
                return;
            removeFromEpoll(proc.ptyMaster);
            fdToId.erase(proc.ptyMaster);
            ::close(proc.ptyMaster);
            proc.ptyMaster = -1;
        }

        // --- epoll event handlers ---------------------------------------------

        void handleParentReadable()
        {
            bool ok = io.readAvailable(
                [this](FdJsonIo::ResultType const& result)
                {
                    if (!result)
                    {
                        spdlog::error("[worker] IPC read error: {}", result.error());
                        running = false;
                        return;
                    }
                    handleCommand(*result);
                }
            );
            if (!ok)
                running = false;
        }

        void handleSignal()
        {
            signalfd_siginfo info{};
            ssize_t nread = ::read(sigFd, &info, sizeof(info));
            if (nread != static_cast<ssize_t>(sizeof(info)))
                return;

            // Reap all children that have already exited.
            int status = 0;
            pid_t pid = ::waitpid(-1, &status, WNOHANG);
            while (pid > 0)
            {
                bool found = false;
                for (auto it = procs.begin(); it != procs.end(); ++it)
                {
                    if (it->second.pid != pid)
                        continue;

                    found = true;
                    closePtyMaster(it->second);

                    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    spdlog::info("[worker] process exited id='{}' pid={} code={}", it->first, pid, code);
                    sendJson({{"id", it->first}, {"type", "exit"}, {"code", code}});
                    procs.erase(it);
                    break;
                }
                if (!found)
                    spdlog::warn("[worker] SIGCHLD for unknown pid={}", pid);

                pid = ::waitpid(-1, &status, WNOHANG);
            }
        }

        void handlePtyReadable(int fd)
        {
            auto fdIt = fdToId.find(fd);
            if (fdIt == fdToId.end())
                return;
            const std::string& procId = fdIt->second;

            char buf[4096];
            ssize_t nread = ::read(fd, buf, sizeof(buf));
            if (nread > 0)
            {
                spdlog::trace("[worker] PTY {} byte(s) for id='{}'", nread, procId);
                sendJson({
                    {"id", procId},
                    {"type", "stdout"},
                    {"data", Roar::base64Encode(std::string{buf, static_cast<std::size_t>(nread)})},
                });
            }
            else if (nread == 0 || (nread == -1 && errno != EAGAIN && errno != EINTR))
            {
                // EIO or unexpected close — remove from epoll; SIGCHLD will send exit
                spdlog::debug("[worker] PTY EIO/close for id='{}' fd={}", procId, fd);
                auto procIt = procs.find(procId);
                if (procIt != procs.end())
                    closePtyMaster(procIt->second);
            }
        }

        // --- command dispatch -------------------------------------------------

        void handleCommand(nlohmann::json const& msg)
        {
            const auto cmd = msg.value("command", std::string{});
            const auto procId = msg.value("id", std::string{});

            spdlog::debug("[worker] command='{}' id='{}'", cmd, procId);
            try
            {
                if (cmd == "quit")
                    running = false;
                else if (cmd == "spawn")
                    handleSpawn(procId, msg["payload"]);
                else if (cmd == "stdin")
                    handleStdin(procId, msg["payload"]);
                else if (cmd == "resize")
                    handleResize(procId, msg["payload"]);
                else if (cmd == "kill")
                    handleKill(procId);
                else if (cmd == "listProcesses")
                    handleListProcesses(procId, msg["payload"]);
                else
                    spdlog::warn("[worker] unknown command '{}' id='{}'", cmd, procId);
            }
            catch (std::exception const& exc)
            {
                spdlog::error("[worker] command exception: cmd='{}' id='{}' what='{}'", cmd, procId, exc.what());
                sendJson({{"id", procId}, {"type", "error"}, {"message", exc.what()}});
            }
        }

        void handleSpawn(std::string const& procId, nlohmann::json const& payload)
        {
            spdlog::info("[worker] spawn id='{}'", procId);

            const auto exe = payload["exe"].get<std::string>();
            const auto args = payload["args"].get<std::vector<std::string>>();
            const auto envMap = payload["env"].get<std::unordered_map<std::string, std::string>>();
            const auto termy = payload.value("termios", nlohmann::json{}).get<Persistence::Termios>();

            // Build termios
            Persistence::Termios saneTty{
                .inputFlags = Persistence::Termios::InputFlags{}.saneDefaults(),
                .outputFlags = Persistence::Termios::OutputFlags{}.saneDefaults(),
                .controlFlags = Persistence::Termios::ControlFlags{}.saneDefaults(),
                .localFlags = Persistence::Termios::LocalFlags{}.saneDefaults(),
                .cc = Persistence::Termios::CC{},
            };
            struct termios ttyOpts{
                .c_iflag = termy.inputFlags.assemble(),
                .c_oflag = termy.outputFlags.assemble(),
                .c_cflag = termy.controlFlags.assemble(),
                .c_lflag = termy.localFlags.assemble(),
                .c_line = 0,
                .c_cc = {},
                .c_ispeed = 0,
                .c_ospeed = 0,
            };
            {
                const auto& src = termy.cc ? *termy.cc : *saneTty.cc;
                std::vector<unsigned char> cc = src.assemble();
                for (std::size_t idx = 0; idx < cc.size() && idx < NCCS; ++idx)
                    ttyOpts.c_cc[idx] = cc[idx];
            }
            // PTYs have no real baud rate, but many programs (stty, login_tty)
            // treat speed 0 as "hang up".  Use 38400 as the conventional default.
            ::cfsetispeed(&ttyOpts, termy.iSpeed && *termy.iSpeed ? *termy.iSpeed : B38400);
            ::cfsetospeed(&ttyOpts, termy.oSpeed && *termy.oSpeed ? *termy.oSpeed : B38400);

            struct winsize ws{.ws_row = 30, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};

            int master = -1;
            int slave = -1;
            if (::openpty(&master, &slave, nullptr, &ttyOpts, &ws) == -1)
            {
                spdlog::error("[worker] openpty failed for id='{}': {}", procId, std::strerror(errno));
                sendJson(
                    {{"id", procId}, {"type", "error"}, {"message", std::string{"openpty: "} + std::strerror(errno)}}
                );
                return;
            }

            // Build argv / envp on the stack so the vectors stay alive through execve
            std::vector<const char*> argv;
            argv.reserve(1 + args.size() + 1);
            argv.push_back(exe.c_str());
            for (const auto& arg : args)
                argv.push_back(arg.c_str());
            argv.push_back(nullptr);

            std::vector<std::string> envStrs;
            envStrs.reserve(envMap.size() + 1);
            bool hasTerm = false;
            for (const auto& [key, val] : envMap)
            {
                if (key == "TERM" && !val.empty() && val != "unknown")
                    hasTerm = true;
                envStrs.push_back(key + "=" + val);
            }
            // Ensure ncurses-based apps can determine terminal capabilities.
            if (!hasTerm)
                envStrs.push_back("TERM=xterm-256color");
            std::vector<const char*> envp;
            envp.reserve(envStrs.size() + 1);
            for (const auto& str : envStrs)
                envp.push_back(str.c_str());
            envp.push_back(nullptr);

            pid_t pid = ::fork();
            if (pid < 0)
            {
                ::close(master);
                ::close(slave);
                spdlog::error("[worker] fork failed for id='{}': {}", procId, std::strerror(errno));
                sendJson(
                    {{"id", procId}, {"type", "error"}, {"message", std::string{"fork: "} + std::strerror(errno)}}
                );
                return;
            }

            if (pid == 0)
            {
                // Child: set up terminal and exec
                ::close(master);
                if (::login_tty(slave) == -1)
                    ::_exit(127);
                ::execve(exe.c_str(), const_cast<char* const*>(argv.data()), const_cast<char* const*>(envp.data()));
                ::_exit(127);
            }

            // Parent (worker): slave fd no longer needed
            ::close(slave);

            // Make master non-blocking for epoll edge case safety
            int flags = ::fcntl(master, F_GETFL, 0);
            ::fcntl(master, F_SETFL, flags | O_NONBLOCK);

            addToEpoll(master, EPOLLIN);
            procs.emplace(procId, WProc{pid, master, procId});
            fdToId.emplace(master, procId);

            spdlog::info("[worker] spawned pid={} id='{}'", pid, procId);
            sendJson({
                {"id", procId},
                {"type", "open"},
                {"responseId", payload["responseId"].get<std::string>()},
            });
        }

        void handleStdin(std::string const& procId, nlohmann::json const& payload)
        {
            auto it = procs.find(procId);
            if (it == procs.end() || it->second.ptyMaster < 0)
            {
                spdlog::warn("[worker] stdin: no process for id='{}'", procId);
                return;
            }
            const auto decoded = Roar::base64Decode(payload["data"].get<std::string>());
            spdlog::trace("[worker] stdin {} byte(s) for id='{}'", decoded.size(), procId);
            const char* ptr = decoded.data();
            std::size_t remaining = decoded.size();
            while (remaining > 0)
            {
                ssize_t written = ::write(it->second.ptyMaster, ptr, remaining);
                if (written > 0)
                {
                    ptr += written;
                    remaining -= static_cast<std::size_t>(written);
                }
                else if (written == -1 && errno == EINTR)
                {
                    /* retry */
                }
                else
                {
                    spdlog::warn("[worker] stdin write error for id='{}': {}", procId, std::strerror(errno));
                    break;
                }
            }
        }

        void handleResize(std::string const& procId, nlohmann::json const& payload)
        {
            auto it = procs.find(procId);
            if (it == procs.end() || it->second.ptyMaster < 0)
            {
                spdlog::warn("[worker] resize: no process for id='{}'", procId);
                return;
            }
            struct winsize ws{
                .ws_row = payload["rows"].get<unsigned short>(),
                .ws_col = payload["cols"].get<unsigned short>(),
                .ws_xpixel = 0,
                .ws_ypixel = 0,
            };
            spdlog::debug("[worker] resize cols={} rows={} for id='{}'", ws.ws_col, ws.ws_row, procId);
            ::ioctl(it->second.ptyMaster, TIOCSWINSZ, &ws);
        }

        void handleListProcesses(std::string const& procId, nlohmann::json const& payload)
        {
            const auto responseId = payload.value("responseId", std::string{});

            const auto it = procs.find(procId);
            if (it == procs.end() || it->second.ptyMaster < 0)
            {
                sendJson(
                    {{"id", procId},
                        {"type", "listProcesses"},
                        {"responseId", responseId},
                        {"error", "no such process"}}
                );
                return;
            }

            char slaveName[256];
            if (::ptsname_r(it->second.ptyMaster, slaveName, sizeof(slaveName)) != 0)
            {
                sendJson(
                    {{"id", procId},
                        {"type", "listProcesses"},
                        {"responseId", responseId},
                        {"error", std::string{"ptsname_r: "} + std::strerror(errno)}}
                );
                return;
            }

            nlohmann::json procsList = nlohmann::json::array();
            try
            {
                for (const auto& entry : std::filesystem::directory_iterator("/proc"))
                {
                    try
                    {
                        if (!entry.is_directory())
                            continue;
                        const std::string pidStr = entry.path().filename().string();
                        if (!std::all_of(
                                pidStr.begin(),
                                pidStr.end(),
                                [](unsigned char chr)
                                {
                                    return std::isdigit(chr);
                                }
                            ))
                            continue;
                        const auto fdPath = entry.path() / "fd" / "0";
                        if (!std::filesystem::is_symlink(fdPath))
                            continue;
                        std::error_code ec;
                        const auto target = std::filesystem::read_symlink(fdPath, ec);
                        if (ec || target.string() != slaveName)
                            continue;
                        std::ifstream cmdlineFile{entry.path() / "cmdline"};
                        if (!cmdlineFile)
                            continue;
                        std::string cmdline;
                        std::getline(cmdlineFile, cmdline, '\0');
                        procsList.push_back({{"pid", std::stoi(pidStr)}, {"cmdline", cmdline}});
                    }
                    catch (...)
                    {
                        continue;
                    }
                }
            }
            catch (...)
            {}

            std::sort(
                procsList.begin(),
                procsList.end(),
                [](nlohmann::json const& a, nlohmann::json const& b)
                {
                    return a["pid"].get<int>() < b["pid"].get<int>();
                }
            );

            sendJson({{"id", procId}, {"type", "listProcesses"}, {"responseId", responseId}, {"procs", procsList}});
        }

        void handleKill(std::string const& procId)
        {
            auto it = procs.find(procId);
            if (it == procs.end())
            {
                spdlog::warn("[worker] kill: no process for id='{}'", procId);
                return;
            }
            spdlog::info("[worker] SIGKILL pid={} id='{}'", it->second.pid, procId);
            ::kill(it->second.pid, SIGKILL);
        }

        // --- main event loop --------------------------------------------------

        void run()
        {
            epollFd = ::epoll_create1(EPOLL_CLOEXEC);
            if (epollFd == -1)
            {
                spdlog::error("[worker] epoll_create1: {}", std::strerror(errno));
                return;
            }

            // Make IPC read fd non-blocking
            int flags = ::fcntl(readFd, F_GETFL, 0);
            ::fcntl(readFd, F_SETFL, flags | O_NONBLOCK);

            addToEpoll(readFd, EPOLLIN);

            // Block SIGCHLD and receive it via signalfd
            sigset_t mask{};
            ::sigemptyset(&mask);
            ::sigaddset(&mask, SIGCHLD);
            ::sigprocmask(SIG_BLOCK, &mask, nullptr);
            sigFd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
            if (sigFd == -1)
            {
                spdlog::error("[worker] signalfd: {}", std::strerror(errno));
                ::close(epollFd);
                return;
            }
            addToEpoll(sigFd, EPOLLIN);

            spdlog::info("[worker] entering epoll loop");

            epoll_event events[32];
            while (running)
            {
                int nfds = ::epoll_wait(epollFd, events, 32, -1);
                if (nfds == -1)
                {
                    if (errno == EINTR)
                        continue;
                    spdlog::error("[worker] epoll_wait: {}", std::strerror(errno));
                    break;
                }

                for (int idx = 0; idx < nfds && running; ++idx)
                {
                    int fd = events[idx].data.fd;
                    if (fd == readFd)
                        handleParentReadable();
                    else if (fd == sigFd)
                        handleSignal();
                    else
                        handlePtyReadable(fd);
                }
            }

            spdlog::info("[worker] epoll loop exited");
            ::close(sigFd);
            ::close(epollFd);
        }
    };

    [[noreturn]] void workerMain(int readFd, int writeFd)
    {
        {
            auto logPath = Nui::resolvePath("%state_home2%/nui-sftp/logs/worker.log");
            std::error_code mkdirEc;
            std::filesystem::create_directories(logPath.parent_path(), mkdirEc);
            auto fileSink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath.string(), 2 * 1024 * 1024, 3, true);
            fileSink->set_level(spdlog::level::trace);
            auto logger = std::make_shared<spdlog::logger>("worker", std::move(fileSink));
            logger->set_level(spdlog::level::trace);
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [worker] %v");
            logger->flush_on(spdlog::level::trace);
            spdlog::set_default_logger(std::move(logger));
        }

        spdlog::info("[worker] started pid={}", static_cast<int>(::getpid()));

        WorkerState state{readFd, writeFd};
        state.run();

        spdlog::info("[worker] shutting down");
        spdlog::shutdown();
        ::_exit(0);
    }

} // namespace

// =============================================================================
// Parent-side ForkPool implementation — still uses boost::asio.
// =============================================================================

struct ForkPool::Implementation : std::enable_shared_from_this<Implementation>
{
    boost::asio::strand<boost::asio::any_io_executor> strand;
    pid_t workerPid{-1};
    JsonProcessIo<boost::asio::readable_pipe, boost::asio::writable_pipe> io;
    std::function<void(nlohmann::json const&)> onMessage;
    std::mutex callbackMutex;

    Implementation(boost::asio::any_io_executor exec, std::function<void(nlohmann::json const&)> callback)
        : strand{std::move(exec)}
        , io(boost::asio::readable_pipe{strand}, boost::asio::writable_pipe{strand})
        , onMessage{std::move(callback)}
    {}

    void asyncRead()
    {
        io.enterReadLoop(
            [this](auto const& result)
            {
                if (result)
                {
                    std::function<void(nlohmann::json const&)> handler;
                    {
                        std::scoped_lock lock{callbackMutex};
                        handler = onMessage;
                    }
                    if (handler)
                        handler(result.value());
                }
                else
                {
                    spdlog::error("Error reading from worker: {}", result.error());
                }
            }
        );
    }

    void enqueue(nlohmann::json const& payload)
    {
        spdlog::debug("Enqueueing message to worker: {}", payload.dump());
        io.write(
            payload,
            [](bool success)
            {
                if (!success)
                    spdlog::error("Error writing to worker");
            }
        );
    }
};

// ---------------------------------------------------------------------------

ForkPool::ForkPool() = default;

ForkPool::~ForkPool()
{
    stop();
}

ForkPool::ForkPool(ForkPool&&) noexcept = default;

ForkPool& ForkPool::operator=(ForkPool&& other) noexcept
{
    if (this != &other)
    {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

void ForkPool::start(boost::asio::any_io_executor executor, std::function<void(nlohmann::json const&)> onMessage)
{
    impl_ = std::make_shared<Implementation>(executor, std::move(onMessage));

    int parentToWorker[2];
    int workerToParent[2];

    if (::pipe2(parentToWorker, O_CLOEXEC) == -1)
        throw std::system_error{errno, std::system_category(), "pipe2(parentToWorker)"};

    if (::pipe2(workerToParent, O_CLOEXEC) == -1)
    {
        ::close(parentToWorker[0]);
        ::close(parentToWorker[1]);
        throw std::system_error{errno, std::system_category(), "pipe2(workerToParent)"};
    }

    auto& ctx = boost::asio::query(executor, boost::asio::execution::context);
    ctx.notify_fork(boost::asio::execution_context::fork_prepare);

    pid_t pid = ::fork();

    if (pid == -1)
    {
        ctx.notify_fork(boost::asio::execution_context::fork_parent);
        ::close(parentToWorker[0]);
        ::close(parentToWorker[1]);
        ::close(workerToParent[0]);
        ::close(workerToParent[1]);
        throw std::system_error{errno, std::system_category(), "fork"};
    }

    if (pid == 0)
    {
        // Child: reset parent's asio context copy, then hand off to worker
        ctx.notify_fork(boost::asio::execution_context::fork_child);

        ::close(parentToWorker[1]);
        ::close(workerToParent[0]);

        int devNull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devNull != -1)
        {
            ::dup2(devNull, STDERR_FILENO);
            ::close(devNull);
        }

        workerMain(parentToWorker[0], workerToParent[1]); // [[noreturn]]
    }

    // Parent
    ctx.notify_fork(boost::asio::execution_context::fork_parent);
    impl_->workerPid = pid;

    ::close(parentToWorker[0]);
    ::close(workerToParent[1]);

    boost::system::error_code err;
    impl_->io.output().assign(parentToWorker[1], err);
    if (err)
    {
        ::close(parentToWorker[1]);
        ::close(workerToParent[0]);
        throw boost::system::system_error{err, "assign toWorker"};
    }
    impl_->io.input().assign(workerToParent[0], err);
    if (err)
    {
        ::close(workerToParent[0]);
        throw boost::system::system_error{err, "assign fromWorker"};
    }

    impl_->asyncRead();
}

void ForkPool::setMessageHandler(std::function<void(nlohmann::json const&)> handler)
{
    if (!impl_)
        return;
    std::scoped_lock lock{impl_->callbackMutex};
    impl_->onMessage = std::move(handler);
}

void ForkPool::stop()
{
    if (!impl_ || impl_->workerPid == -1)
        return;

    boost::system::error_code ignored;
    impl_->io.input().close(ignored);
    impl_->io.output().close(ignored);

    for (int attempt = 0; attempt < 50; ++attempt)
    {
        int status = 0;
        pid_t ret = ::waitpid(impl_->workerPid, &status, WNOHANG);
        if (ret == impl_->workerPid || ret == -1)
        {
            impl_->workerPid = -1;
            return;
        }
        ::usleep(10'000);
    }

    ::kill(impl_->workerPid, SIGTERM);
    ::waitpid(impl_->workerPid, nullptr, 0);
    impl_->workerPid = -1;
}

void ForkPool::send(nlohmann::json const& message)
{
    if (!impl_ || impl_->workerPid == -1)
        return;
    impl_->enqueue(message);
}
