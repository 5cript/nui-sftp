#pragma once

#include <backend/process/process.hpp>
#include <backend/process/environment.hpp>

#include <persistence/state/termios.hpp>

#include <nui/rpc.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <chrono>
#include <vector>

class ForkPool;

class ProcessStore
{
  public:
    ProcessStore(
        boost::asio::any_io_executor executor,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        ForkPool* forkPool = nullptr
    );
    ~ProcessStore();

    void registerRpc(Nui::Window& wnd, Nui::RpcHub& hub);

    std::string emplace(
        std::string const& command,
        std::vector<std::string> const& arguments,
        Environment const& environment,
        Persistence::Termios const& termios,
        bool isPty = false,
        std::chrono::seconds defaultExitWaitTimeout = std::chrono::seconds{10}
    );

    std::shared_ptr<Process> operator[](std::string const& id) const
    {
        auto iter = processes_.find(id);
        if (iter == processes_.end())
            return nullptr;
        return iter->second;
    }

    void notifyChildExit(Nui::RpcHub& hub, long long pid);
    void notifyChildExit(Nui::RpcHub& hub, std::string const& id);

#ifndef _WIN32
    /**
     * @brief Spawn a detached process via the fork-pool worker (no piped stdio, fire-and-forget).
     * @param exe  Executable path.
     * @param args Command-line arguments.
     */
    void spawnDetached(std::string const& exe, std::vector<std::string> const& args);
#endif

    void pruneDeadProcesses();

  private:
#ifndef _WIN32
    struct ForkPoolProcess
    {
        std::string stdoutReceptacle;
        std::string stderrReceptacle;
    };
    void handleWorkerMessage(nlohmann::json const& message);
#endif

    boost::asio::any_io_executor executor_;
    Nui::Window* wnd_;
    Nui::RpcHub* hub_;
    std::unordered_map<std::string, std::shared_ptr<Process>> processes_;
    boost::uuids::random_generator uuidGenerator_;
#ifndef _WIN32
    ForkPool* forkPool_{nullptr};
    std::unordered_map<std::string, ForkPoolProcess> forkPoolProcesses_;
#endif
};