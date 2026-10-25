#include <utility/node/node.hpp>
#include <utility/error_message.hpp>
#include <paths/native.hpp>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <boost/process.hpp>
#pragma clang diagnostic pop

#include <boost/asio/deadline_timer.hpp>
#include <fmt/format.h>

#include <array>
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
        const auto commandWithNewline = fmt::format("{}\r\n", j.dump());
#else
        const auto commandWithNewline = fmt::format("{}\n", j.dump());
#endif
        boost::system::error_code ec;
        boost::asio::write(stdinPipe, boost::asio::buffer(commandWithNewline), ec);
        if (ec)
        {
            std::cerr << "Failed to write command: " << Utility::errorMessage(ec) << std::endl;
            return;
        }
        // Node on Windows cannot finish exiting while a read on its stdin pipe is pending.
        if (command == "exit")
            stdinPipe.close(ec);
    }

    bool NodeProcessResult::command(std::string const& command, nlohmann::json const& arguments)
    {
        return query(command, arguments).has_value();
    }

    std::optional<nlohmann::json>
    NodeProcessResult::query(std::string const& command, nlohmann::json const& arguments)
    {
        if (port == 0 || killed || code != 0)
            return std::nullopt;

        nlohmann::json commandObject = arguments.is_object() ? arguments : nlohmann::json::object();
        commandObject["command"] = command;
        commandObject["ack"] = true;
#ifdef _WIN32
        const auto commandWithNewline = fmt::format("{}\r\n", commandObject.dump());
#else
        const auto commandWithNewline = fmt::format("{}\n", commandObject.dump());
#endif

        boost::system::error_code ec;
        boost::asio::write(stdinPipe, boost::asio::buffer(commandWithNewline), ec);
        if (ec)
        {
            std::cerr << "Failed to write command: " << Utility::errorMessage(ec) << std::endl;
            return std::nullopt;
        }

        std::string received{};
        std::array<char, 256> buffer{};
        while (received.find('\n') == std::string::npos)
        {
            const auto readAmount = stdoutPipe.read_some(boost::asio::buffer(buffer), ec);
            if (ec || readAmount == 0)
            {
                std::cerr << "Failed to read command acknowledgement: " << Utility::errorMessage(ec) << std::endl;
                return std::nullopt;
            }
            received.append(buffer.data(), readAmount);
        }
        const auto acknowledgement =
            nlohmann::json::parse(received.substr(0, received.find('\n')), nullptr, false);
        if (!acknowledgement.is_object() || acknowledgement.value("ack", "") != command)
            return std::nullopt;
        return acknowledgement.value("reply", nlohmann::json{});
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
            std::cerr << "node process wait failed: " << Utility::errorMessage(errc) << std::endl;
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
            std::cerr << "Failed to terminate process: " << Utility::errorMessage(e) << std::endl;
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

        const auto command = fmt::format("\"cd {} && npm install\"", Paths::nativeToUtf8(directory));
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
            bp2::process_start_dir{directory.native()}};
        child.wait();
    }

    void drainStandardError(std::shared_ptr<NodeProcessResult> const& result)
    {
        result->stderrPipe.async_read_some(
            boost::asio::buffer(result->stderrBuffer),
            [weak = std::weak_ptr{result}](boost::system::error_code const& ec, std::size_t)
            {
                if (ec)
                    return;
                if (auto result = weak.lock())
                    drainStandardError(result);
            }
        );
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
                bp2::process_start_dir{isolateDirectory.path().native()},
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
                    std::cerr << "Failed to terminate process: " << Utility::errorMessage(e) << std::endl;
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
                std::cerr << "Failed to read port: " << Utility::errorMessage(ec) << std::endl;
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
            std::cerr << "Failed to parse port object: " << Utility::errorMessage(e) << std::endl;
            return result;
        }

        // Windows writes to pipes synchronously: once nobody reads stderr and its small buffer is full, the
        // process blocks for good and only the kill timer ends it.
        drainStandardError(result);
        return result;
    }
}