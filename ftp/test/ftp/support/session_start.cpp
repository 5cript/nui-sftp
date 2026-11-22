#include <ftp/support/session_start.hpp>
#include <ftp/engine/resolver.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <fmt/format.h>

#include <future>
#include <utility>

namespace FileTransferProtocol::Test
{
    std::expected<StartedSession, std::string> startSession(RemoteIo::ProcessingThread& thread, SessionStart start)
    {
        const auto parallelTransfers = start.pool.transferConnections;
        EngineOptions engineOptions{};
        engineOptions.pool = std::move(start.pool);
        auto engine =
            Engine::create(start.executor, std::make_shared<Resolver>(), start.gate, std::move(engineOptions));
        auto login = boost::asio::co_spawn(engine->strand(), engine->pool()->start(), boost::asio::use_future);
        if (login.wait_for(start.loginLimit) != std::future_status::ready)
        {
            engine->close();
            return std::unexpected(std::string{"The login did not end"});
        }
        const auto loggedIn = login.get();
        if (!loggedIn)
        {
            engine->close();
            return std::unexpected(fmt::format("The login failed: {}", loggedIn.error().toString()));
        }
        start.dialect->learnFrom(loggedIn->features);
        const auto probesEntries = start.dialect->probesEntries();
        auto fileSystem = FtpFileSystem::create(
            FileSystemSession{
                .engine = engine,
                .dialect = std::move(start.dialect),
                .homeDirectory = loggedIn->homeDirectory,
                .parallelTransfers = parallelTransfers,
                .probesEntries = probesEntries,
            },
            thread.createStrand(),
            std::move(start.fileSystem)
        );
        return StartedSession{.engine = std::move(engine), .fileSystem = std::move(fileSystem)};
    }
}
