#pragma once

#include <gtest/gtest.h>

#include <utility/node/node.hpp>
#include <ssh/session.hpp>
#include <nui/utility/scope_exit.hpp>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <boost/process.hpp>
#pragma clang diagnostic pop

#include <filesystem>
#include <fstream>
#include <chrono>

extern std::filesystem::path programDirectory;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"

static const char sshServerBashEmulator[] = {
#embed "./test_ssh2_servers/dist/shell.mjs"
    ,
    '\0',
};

static const char sftpServer[] = {
#embed "./test_ssh2_servers/dist/sftp.mjs"
    ,
    '\0',
};

static const char privateKey[] = {
#embed "./test_ssh2_servers/key.private"
    ,
    '\0',
};

static const char publicKey[] = {
#embed "./test_ssh2_servers/key.public"
    ,
    '\0',
};

#pragma clang diagnostic pop

namespace SecureShell::Test
{
    class CommonFixture : public ::testing::Test
    {
      public:
        static constexpr std::chrono::seconds connectTimeout{2};

      protected:
        void SetUp() override
        {
            std::ofstream privateKeyFile{programDirectory / "temp" / "key.private", std::ios_base::binary};
            privateKeyFile.write(privateKey, std::strlen(privateKey));
        }

        std::pair<std::shared_ptr<NodeProcessResult>, std::thread> createServer(std::string const& source)
        {
            std::shared_ptr<NodeProcessResult> result{};
            std::promise<void> processResultAvailable{};
            std::thread processThread{[this, &result, &processResultAvailable, source]() mutable
                {
                    result = nodeProcess(pool_.get_executor(), isolateDirectory_, source);
                    auto resultShareCopy = result;
                    processResultAvailable.set_value();
                    if (resultShareCopy->mainModule)
                    {
                        resultShareCopy->wait();
                    }
                    else
                    {
                        // ???
                        throw std::runtime_error("No main module, why");
                    }
                }};
            processResultAvailable.get_future().wait();
            if (result->port == 0)
            {
                if (processThread.joinable())
                    processThread.join();
                return {nullptr, {}};
            }
            return {result, std::move(processThread)};
        }

        auto createSshServer()
        {
            return createServer(sshServerBashEmulator);
        }

        auto createSftpServer()
        {
            return createServer(sftpServer);
        }

        auto
        getSessionOptions(unsigned short port, std::string const& user = "test", std::string const& host = "127.0.0.1")
        {
            return Persistence::SshSessionOptions{
                .host = host,
                .port = port,
                .user = user,
                .sshOptions = Persistence::SshOptions{
                    .connectTimeoutSeconds = connectTimeout.count(),
                },
            };
        }

      public:
        auto makePasswordTestSession(unsigned short port)
        {
            return makeSession(
                getSessionOptions(port),
                +[](char const*, char* buf, std::size_t length, int, int, void*)
                {
                    static constexpr std::string_view pw = "test";
                    std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                    return 0;
                },
                nullptr,
                nullptr,
                nullptr,
                []()
                {
                    std::cout << "connection lost." << std::endl;
                }
            );
        }

      protected:
        void channelStartReading(
            std::shared_ptr<SecureShell::Channel> const& channel,
            std::function<void(std::string const&)> onStdout = {},
            std::function<void(std::string const&)> onStderr = {},
            std::function<void()> onExit = {}
        )
        {
            channel->startReading(
                onStdout ? onStdout : [](std::string const&) {},
                onStderr ? onStderr : [](std::string const) {},
                [onExit = std::move(onExit)]()
                {
                    if (onExit)
                        onExit();
                }
            );
        }

        void channelStartReading(
            std::shared_ptr<SecureShell::Channel> const& channel,
            std::promise<void>& awaiter,
            std::function<void(std::string const&)> onStdout = {},
            std::function<void(std::string const&)> onStderr = {},
            std::function<void()> onExit = {}
        )
        {
            channelStartReading(
                channel,
                std::move(onStdout),
                std::move(onStderr),
                [onExit = std::move(onExit), &awaiter]()
                {
                    if (onExit)
                        onExit();
                    awaiter.set_value();
                }
            );
        }

        Utility::TemporaryDirectory isolateDirectory_{programDirectory / "temp", false};
        boost::asio::thread_pool pool_{1};
        nlohmann::json defaultPackageJson_ = nlohmann::json({
            {"name", "test"},
            {"version", "1.0.0"},
            {"description", "test"},
            {"main", "main.mjs"},
            {
                "dependencies",
                {
                    {"ssh2", "1.16.0"},
                    {"blessed", "0.1.81"},
                    {"nanoid", "5.1.0"},
                    {"@xterm/headless", "5.6.0-beta.98"},
                    {"minimist", "1.2.8"},
                },
            },
        });
    };
}

#define CREATE_SERVER_AND_JOINER(name) \
    auto [serverStartResult, processThread] = create##name##Server(); \
    ASSERT_TRUE(serverStartResult); \
    auto joiner = Nui::ScopeExit{[&]() noexcept \
        { \
            serverStartResult->command("exit"); \
            if (processThread.joinable()) \
                processThread.join(); \
        }};