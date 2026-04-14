#include <utility/node/node.hpp>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <boost/process.hpp>
#pragma clang diagnostic pop

#include <boost/asio/deadline_timer.hpp>

#include <fstream>
#include <iostream>
#include <limits>

constexpr static std::string_view node = NODE_EXECUTABLE;
using namespace std::string_literals;

namespace SecureShell::Test
{
    namespace
    {
        boost::process::process* process(NodeProcessResult& result)
        {
            return static_cast<boost::process::process*>(result.mainModule.get());
        }
    }

    namespace bp2 = boost::process::v2;

    void NodeProcessResult::command(std::string const& command)
    {
        if (port == 0 || killed || code != 0)
            return;

        nlohmann::json j = {
            {"command", command},
        };
#ifdef _WIN32
        const std::string commandWithNewline = j.dump() + "\r\n";
#else
        const std::string commandWithNewline = j.dump() + "\n";
#endif
        boost::system::error_code ec;
        boost::asio::write(stdinPipe, boost::asio::buffer(commandWithNewline), ec);
        if (ec)
        {
            std::cerr << "Failed to write command: " << ec.message() << std::endl;
            return;
        }
    }

    int NodeProcessResult::wait()
    {
        if (!mainModule)
            return std::numeric_limits<int>::min();

        // boost::process v2 may install an internal async SIGCHLD reaper via the
        // executor. If that reaper wins the race against this synchronous wait(),
        // waitid returns ECHILD and the throwing overload aborts the process.
        // Use the error_code overload and treat ECHILD as "already reaped".
        boost::system::error_code errc{};
        const int exitCode = process(*this)->wait(errc);
        if (errc && errc != boost::system::errc::no_child_process)
        {
            std::cerr << "node process wait failed: " << errc.message() << std::endl;
        }
        return exitCode;
    }

    void NodeProcessResult::terminate()
    {
        if (killed)
            return;
        killed = true;
        try
        {
            process(*this)->terminate();
        }
        catch (std::exception const& e)
        {
            std::cerr << "Failed to terminate process: " << e.what() << std::endl;
        }
    }

    void npmInstall(
        boost::asio::any_io_executor executor,
        std::filesystem::path const& directory,
        nlohmann::json const& packageJson)
    {
        auto packageJsonPath = directory / "package.json";
        {
            std::ofstream packageJsonFile{packageJsonPath};
            packageJsonFile << packageJson.dump(4);
        }

        const auto command = "\"cd "s + directory.generic_string() + " && npm install\"";
#ifdef _WIN32
        const auto shell = MSYS2_BASH;
        const auto npmShellArgs = std::vector<std::string>{
            "--login",
            "-i",
            "-c",
            std::string{command},
        };
#else
        const auto shell = "/bin/bash";
        const auto npmShellArgs = std::vector<std::string>{
            "-c",
            std::string{command},
        };
#endif

        auto child = boost::process::v2::process{
            executor,
            shell,
            npmShellArgs,
            bp2::process_environment{bp2::environment::current()},
            bp2::process_start_dir{directory.generic_string()}};
        child.wait();
    }

    std::shared_ptr<NodeProcessResult> nodeProcess(
        boost::asio::any_io_executor executor,
        Utility::TemporaryDirectory const& isolateDirectory,
        std::string const& program)
    {
        using namespace std::string_literals;

        {
            std::ofstream programFile{isolateDirectory.path() / "main.mjs", std::ios_base::binary};
            programFile.write(program.data(), program.size());
        }

        const auto nodeExecutable = boost::process::v2::filesystem::path{std::string{node}};

        auto result = std::make_shared<NodeProcessResult>(
            boost::asio::steady_timer{executor, processKillTimer},
            boost::asio::writable_pipe{executor},
            boost::asio::readable_pipe{executor},
            boost::asio::readable_pipe{executor},
            std::unique_ptr<void, void (*)(void*)>{nullptr, +[](void*) {}});

        result->mainModule = std::unique_ptr<void, void (*)(void*)>{
            new boost::process::v2::process{
                executor,
                node,
                std::vector<std::string>{"main.mjs", "--log-file=./log.txt"},
                bp2::process_environment{bp2::environment::current()},
                bp2::process_start_dir{isolateDirectory.path().generic_string()},
                bp2::process_stdio{
                    .in = result->stdinPipe,
                    .out = result->stdoutPipe,
                    .err = result->stderrPipe,
                }},
            [](void* p) {
                delete static_cast<boost::process::v2::process*>(p);
            }};

        result->timer.async_wait([weak = std::weak_ptr{result}](boost::system::error_code const& ec) {
            if (ec)
                return;
            if (auto result = weak.lock())
            {
                result->killed = true;
                try
                {
                    process(*result)->terminate();
                }
                catch (std::exception const& e)
                {
                    std::cerr << "Failed to terminate process: " << e.what() << std::endl;
                }
            }
        });

        nlohmann::json portObject;
        try
        {
            std::string buffer(1024, '\0');
            boost::system::error_code ec;
            int readAmount = result->stdoutPipe.read_some(boost::asio::buffer(buffer), ec);
            if (ec)
            {
                std::cerr << "Failed to read port: " << ec.message() << std::endl;
                return result;
            }
            if (readAmount == 0)
            {
                std::cerr << "Failed to read port: No data" << std::endl;
                return result;
            }
            portObject = nlohmann::json::parse(buffer);
            result->port = portObject["port"].get<unsigned short>();
        }
        catch (std::exception const& e)
        {
            std::cerr << "Failed to parse port object: " << e.what() << std::endl;
            return result;
        }

        return result;
    }
}