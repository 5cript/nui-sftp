#include <backend/process/fork_pool.hpp>

#include <backend/process/json_process_io.hpp>
#include <backend/pty/posix/process_list.hpp>
#include <nui/backend/filesystem/special_paths.hpp>
#include <persistence/state/termios.hpp>
#include <roar/utility/base64.hpp>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#    include <util.h>
#else
#    include <pty.h>
#    include <utmp.h>
#endif

namespace
{
    // =========================================================================
    // Worker-side — runs entirely in the child process.
    // Single-threaded, driven by poll.  No boost, no threads, no mutexes.
    // =========================================================================

    // Write end of the SIGCHLD self-pipe, the handler may only touch async-signal-safe state.
    volatile sig_atomic_t childSignalPipe = -1;

    void onChildSignal(int)
    {
        const int savedErrno = errno;
        const char byte = 0;
        [[maybe_unused]] const auto written = ::write(childSignalPipe, &byte, 1);
        errno = savedErrno;
    }

    bool makePipe(int (&fds)[2], int extraFlags)
    {
        if (::pipe(fds) == -1)
            return false;
        for (const int fd : fds)
        {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (extraFlags != 0)
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | extraFlags);
        }
        return true;
    }

    struct WProc
    {
        pid_t pid;
        int ptyMaster; // -1 once closed
        std::string id;
        // Stdin the pty did not take yet, a macOS pty only buffers about 1 KiB.
        std::string pendingInput{};
    };

    struct WorkerState
    {
        int readFd{-1};
        int writeFd{-1};
        int signalReadFd{-1};
        FdJsonIo io;
        std::unordered_map<std::string, WProc> procs; // id  -> WProc
        std::unordered_map<int, std::string> fdToId; // pty master fd -> id
        bool running{true};

        WorkerState(int readFd_, int writeFd_)
            : readFd{readFd_}
            , writeFd{writeFd_}
            , io{readFd_, writeFd_}
        {}

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
            fdToId.erase(proc.ptyMaster);
            ::close(proc.ptyMaster);
            proc.ptyMaster = -1;
            proc.pendingInput.clear();
        }

        // --- poll event handlers ----------------------------------------------

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
            char drain[64];
            while (::read(signalReadFd, drain, sizeof(drain)) > 0)
            {}

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
                // EIO or unexpected close — stop polling it; SIGCHLD will send exit
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
#ifdef __linux__
                .c_line = 0,
#endif
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

            // Non-blocking so a spurious readiness never stalls the loop
            int flags = ::fcntl(master, F_GETFL, 0);
            ::fcntl(master, F_SETFL, flags | O_NONBLOCK);

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
            it->second.pendingInput += decoded;
            flushInput(it->second);
        }

        /**
         * @brief Writes as much pending stdin as the pty takes, the rest waits for POLLOUT.
         */
        void flushInput(WProc& proc)
        {
            while (!proc.pendingInput.empty() && proc.ptyMaster >= 0)
            {
                const ssize_t written = ::write(proc.ptyMaster, proc.pendingInput.data(), proc.pendingInput.size());
                if (written > 0)
                    proc.pendingInput.erase(0, static_cast<std::size_t>(written));
                else if (written == -1 && errno == EINTR)
                    continue;
                else if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return;
                else
                {
                    spdlog::warn("[worker] stdin write error for id='{}': {}", proc.id, std::strerror(errno));
                    proc.pendingInput.clear();
                    return;
                }
            }
        }

        void handlePtyWritable(int fd)
        {
            auto fdIt = fdToId.find(fd);
            if (fdIt == fdToId.end())
                return;
            auto procIt = procs.find(fdIt->second);
            if (procIt != procs.end())
                flushInput(procIt->second);
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
            for (auto const& process : PTY::listProcessesOnTerminal(slaveName))
                procsList.push_back({{"pid", process.pid}, {"cmdline", process.cmdline}});

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
            // Make IPC read fd non-blocking
            int flags = ::fcntl(readFd, F_GETFL, 0);
            ::fcntl(readFd, F_SETFL, flags | O_NONBLOCK);

            int signalPipe[2];
            if (!makePipe(signalPipe, O_NONBLOCK))
            {
                spdlog::error("[worker] pipe: {}", std::strerror(errno));
                return;
            }
            signalReadFd = signalPipe[0];
            childSignalPipe = signalPipe[1];

            struct sigaction action{};
            action.sa_handler = onChildSignal;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
            ::sigaction(SIGCHLD, &action, nullptr);

            // The parent may have blocked SIGCHLD, children inherit the mask through exec.
            sigset_t mask{};
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);
            ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);

            spdlog::info("[worker] entering poll loop");

            std::vector<pollfd> pollFds;
            while (running)
            {
                pollFds.clear();
                pollFds.push_back({.fd = readFd, .events = POLLIN, .revents = 0});
                pollFds.push_back({.fd = signalReadFd, .events = POLLIN, .revents = 0});
                for (auto const& [fd, id] : fdToId)
                {
                    const auto procIt = procs.find(id);
                    const bool hasPendingInput = procIt != procs.end() && !procIt->second.pendingInput.empty();
                    pollFds.push_back(
                        {.fd = fd, .events = static_cast<short>(POLLIN | (hasPendingInput ? POLLOUT : 0)), .revents = 0}
                    );
                }

                const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), -1);
                if (ready == -1)
                {
                    if (errno == EINTR)
                        continue;
                    spdlog::error("[worker] poll: {}", std::strerror(errno));
                    break;
                }

                for (auto const& entry : pollFds)
                {
                    if (!running)
                        break;
                    if ((entry.revents & (POLLIN | POLLOUT | POLLHUP | POLLERR)) == 0)
                        continue;
                    if (entry.fd == readFd)
                        handleParentReadable();
                    else if (entry.fd == signalReadFd)
                        handleSignal();
                    else
                    {
                        if ((entry.revents & POLLOUT) != 0)
                            handlePtyWritable(entry.fd);
                        if ((entry.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                            handlePtyReadable(entry.fd);
                    }
                }
            }

            spdlog::info("[worker] poll loop exited");
            ::close(signalReadFd);
            ::close(signalPipe[1]);
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

    if (!makePipe(parentToWorker, 0))
        throw std::system_error{errno, std::system_category(), "pipe(parentToWorker)"};

    if (!makePipe(workerToParent, 0))
    {
        ::close(parentToWorker[0]);
        ::close(parentToWorker[1]);
        throw std::system_error{errno, std::system_category(), "pipe(workerToParent)"};
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
