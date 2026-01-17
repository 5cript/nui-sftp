#pragma once

#include <ssh/sftp_session.hpp>
#include <utility/node/node.hpp>

#include <nui/utility/scope_exit.hpp>
#include <gtest/gtest.h>

#include <string>
#include <memory>

using namespace std::chrono_literals;
using namespace std::string_literals;

extern std::filesystem::path programDirectory;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"

static const char sftpServer[] = {
#embed "../../../ssh/test/ssh/test_ssh2_servers/dist/sftp.mjs"
    ,
    '\0',
};

static const char privateKey[] = {
#embed "../../../ssh/test/ssh/test_ssh2_servers/key.private"
    ,
    '\0',
};

static const char publicKey[] = {
#embed "../../../ssh/test/ssh/test_ssh2_servers/key.public"
    ,
    '\0',
};

#pragma clang diagnostic pop

class RealServerTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::ofstream privateKeyFile{programDirectory / "temp" / "key.private", std::ios_base::binary};
        privateKeyFile.write(privateKey, std::strlen(privateKey));
    }

    static constexpr std::chrono::seconds connectTimeout{5};

    std::pair<std::shared_ptr<SecureShell::Test::NodeProcessResult>, std::thread>
    createServer(std::string const& source)
    {
        std::shared_ptr<SecureShell::Test::NodeProcessResult> result{};
        std::promise<void> processResultAvailable{};
        std::thread processThread{[this, &result, &processResultAvailable, source]() mutable
            {
                result = SecureShell::Test::nodeProcess(pool_.get_executor(), isolateDirectory_, source);
                auto resultShareCopy = result;
                processResultAvailable.set_value();
                if (resultShareCopy->mainModule)
                    resultShareCopy->code = resultShareCopy->wait();
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

    auto getSessionOptions(unsigned short port, std::string const& user = "test", std::string const& host = "127.0.0.1")
    {
        return Persistence::SshSessionOptions{
            .sshOptions =
                Persistence::SshOptions{
                    .connectTimeoutSeconds = connectTimeout.count(),
                },
            .host = host,
            .port = port,
            .user = user,
        };
    }

    auto makePasswordTestSession(unsigned short port)
    {
        return SecureShell::makeSession(
            getSessionOptions(port),
            +[](char const*, char* buf, std::size_t length, int, int, void*)
            {
                static constexpr std::string_view pw = "test";
                std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                return 0;
            },
            nullptr,
            nullptr,
            nullptr
        );
    }

    std::pair<std::unique_ptr<SecureShell::Session>, std::shared_ptr<SecureShell::SftpSession>>
    createSftpSession(unsigned short port)
    {
        auto session = makePasswordTestSession(port);
        if (!session.has_value())
            throw std::runtime_error("Failed to create session");

        (*session)->start();
        auto sftpFuture = (*session)->createSftpSession();
        if (sftpFuture.wait_for(10s) != std::future_status::ready)
            throw std::runtime_error("Failed to create sftp session");

        return std::make_pair(std::move(session).value(), sftpFuture.get().value().lock());
    }

  protected:
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

#define CREATE_SERVER_AND_JOINER(name) \
    auto [serverStartResult, processThread] = createServer(name); \
    ASSERT_TRUE(serverStartResult); \
    auto joiner = Nui::ScopeExit{[&]() noexcept \
        { \
            serverStartResult->command("exit"); \
            if (processThread.joinable()) \
                processThread.join(); \
        }};