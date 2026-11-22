#pragma once

#include <ftp/client/metadata.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/engine/task_gate.hpp>
#include <ftp/pool/pool.hpp>
#include <ftp/remote/file_system_session.hpp>
#include <ftp/remote/ftp_file_system.hpp>
#include <remote_io/async/processing_thread.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <string>

namespace FileTransferProtocol::Test
{
    /**
     * @brief What a test session starts with.
     */
    struct SessionStart
    {
        /** @brief The executor of the engine. */
        boost::asio::any_io_executor executor{};
        /** @brief The gate to the ProcessingThread of the session. */
        std::shared_ptr<TaskGate> gate{};
        /** @brief The server and the login, transferConnections is also what the file system offers in parallel. */
        PoolOptions pool{};
        std::shared_ptr<ServerDialect> dialect = std::make_shared<ServerDialect>();
        FtpFileSystemOptions fileSystem{};
        /** @brief How long the login of C0 may take. */
        std::chrono::seconds loginLimit{20};
    };

    struct StartedSession
    {
        std::shared_ptr<Engine> engine{};
        std::shared_ptr<FtpFileSystem> fileSystem{};
    };

    /**
     * @brief Makes the engine of a session, logs in its C0 and makes its file system on a strand of @p thread.
     *
     * @return The session, or why the login failed: the engine is closed again then.
     */
    std::expected<StartedSession, std::string> startSession(RemoteIo::ProcessingThread& thread, SessionStart start);
}
