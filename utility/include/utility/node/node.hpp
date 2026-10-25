#pragma once

#include <boost/asio.hpp>

#include <utility/temporary_directory.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <optional>

namespace SecureShell::Test
{
    constexpr static auto processKillTimer = std::chrono::seconds{10};

    struct NodeProcessResult
    {
        boost::asio::steady_timer timer;
        boost::asio::writable_pipe stdinPipe;
        boost::asio::readable_pipe stdoutPipe;
        boost::asio::readable_pipe stderrPipe;
        std::unique_ptr<void, void (*)(void*)> mainModule;
        std::atomic_bool killed = false;
        unsigned short port = 0;
        int code = 0;
        /** @brief Receives what the process writes to stderr, which is read and dropped. */
        std::array<char, 4096> stderrBuffer{};

        void command(std::string const& command);
        /**
         * @brief Sends a command with arguments and blocks until the process acknowledges it.
         *
         * @param command The command name.
         * @param arguments Extra fields merged into the command object.
         * @return true if the acknowledgement arrived.
         */
        bool command(std::string const& command, nlohmann::json const& arguments);
        /**
         * @brief Sends a command like command() with arguments and returns the reply of its acknowledgement, null
         *        for a command without one.
         *
         * @return Nothing if no acknowledgement arrived.
         */
        std::optional<nlohmann::json> query(std::string const& command, nlohmann::json const& arguments = {});
        void terminate();
        int wait();
    };

    void npmInstall(
        boost::asio::any_io_executor executor,
        std::filesystem::path const& directory,
        nlohmann::json const& packageJson);

    std::shared_ptr<NodeProcessResult> nodeProcess(
        boost::asio::any_io_executor executor,
        Utility::TemporaryDirectory const& isolateDirectory,
        std::string const& program);
}