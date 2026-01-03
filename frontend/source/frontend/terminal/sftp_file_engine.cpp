#include <frontend/terminal/sftp_file_engine.hpp>
#include <log/log.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/api/json.hpp>

struct SftpFileEngine::Implementation
{
    bool wasDisposed = false;
    SshTerminalEngine* engine;
    std::optional<Ids::ChannelId> sftpChannelId{std::nullopt};

    Implementation(SshTerminalEngine* engine)
        : engine{engine}
    {}
};

SftpFileEngine::SftpFileEngine(SshTerminalEngine* engine)
    : impl_{std::make_unique<Implementation>(engine)}
{}
SftpFileEngine::~SftpFileEngine()
{
    if (!moveDetector_.wasMoved())
    {
        dispose([]() {});
    }
}

std::optional<Ids::ChannelId> SftpFileEngine::release()
{
    return std::move(impl_->sftpChannelId);
}

void SftpFileEngine::dispose(std::function<void()> onComplete)
{
    if (!impl_->wasDisposed)
    {
        if (impl_->sftpChannelId)
        {
            Log::info("Closing sftp channel");
            impl_->engine->closeChannel(impl_->sftpChannelId.value(), std::move(onComplete));
        }
    }
    impl_->wasDisposed = true;
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(SftpFileEngine);

void SftpFileEngine::lazyOpen(std::function<void(std::optional<Ids::ChannelId> const&)> const& onOpen)
{
    if (impl_->sftpChannelId)
    {
        onOpen(impl_->sftpChannelId);
        return;
    }

    Log::info("Creating sftp channel");
    impl_->engine->createSftpChannel(
        [this, onOpen](auto const& id)
        {
            impl_->sftpChannelId = id;
            onOpen(id);
        }
    );
}

void SftpFileEngine::listDirectory(
    std::filesystem::path const& path,
    std::function<void(std::optional<std::vector<SharedData::DirectoryEntry>> const&)> onComplete
)
{
    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot list directory, no sftp channel");
                return;
            }

            Log::info("Listing directory: {}", path.generic_string());
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::listDirectory", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Log::info("Received response for listing directory.");
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error") || !val.hasOwnProperty("entries"))
                    {
                        if (val.hasOwnProperty("error"))
                        {
                            Log::error("(Frontend) Failed to list directory: {}", val["error"].as<std::string>());
                        }
                        else
                        {
                            Log::error("(Frontend) Failed to list directory: no entries");
                        }
                        onComplete(std::nullopt);
                        return;
                    }

                    onComplete(nlohmann::json::parse(Nui::JSON::stringify(val))["entries"]);
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}

void SftpFileEngine::createDirectory(std::filesystem::path const& path, std::function<void(bool)> onComplete)
{
    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot create directory, no channel");
                return;
            }

            Log::info("Creating directory: {}", path.generic_string());
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::createDirectory", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to create directory: {}", val["error"].as<std::string>());
                        onComplete(false);
                        return;
                    }

                    onComplete(true);
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}

void SftpFileEngine::createFile(std::filesystem::path const& path, std::function<void(bool)> onComplete)
{
    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot create file, no channel");
                return;
            }

            Log::info("Creating file: {}", path.generic_string());
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::createFile", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to create file: {}", val["error"].as<std::string>());
                        onComplete(false);
                        return;
                    }

                    onComplete(true);
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}

void SftpFileEngine::addDownload(
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    std::function<void(std::optional<Ids::OperationId>)> onOperationCreated,
    bool allowOverwrite,
    bool insertRefresh
)
{
    Log::info("Requesting to add download: {} -> {}", remotePath.generic_string(), localPath.generic_string());
    lazyOpen(
        [this,
            remotePath,
            localPath,
            onOperationCreated = std::move(onOperationCreated),
            allowOverwrite,
            insertRefresh](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot add download, no channel");
                onOperationCreated(std::nullopt);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            Log::info(
                "Adding download (with ID '{}'): {} -> {}",
                operationId.value(),
                remotePath.generic_string(),
                localPath.generic_string()
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addDownload", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add download: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt);
                        return;
                    }
                    onOperationCreated(operationId);
                },
                channelId.value().value(),
                operationId.value(),
                remotePath.generic_string(),
                localPath.generic_string(),
                allowOverwrite,
                insertRefresh
            );
        }
    );
}

void SftpFileEngine::addUpload(
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    std::function<void(std::optional<Ids::OperationId>)> onOperationCreated,
    bool allowOverwrite,
    bool insertRefresh
)
{
    Log::info("Requesting to add upload: {} -> {}", localPath.generic_string(), remotePath.generic_string());
    lazyOpen(
        [this,
            remotePath,
            localPath,
            onOperationCreated = std::move(onOperationCreated),
            allowOverwrite,
            insertRefresh](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot add upload, no channel");
                onOperationCreated(std::nullopt);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            Log::info(
                "Adding upload (with ID '{}'): {} -> {}",
                operationId.value(),
                localPath.generic_string(),
                remotePath.generic_string()
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addUpload", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add upload: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt);
                        return;
                    }
                    onOperationCreated(operationId);
                },
                channelId.value().value(),
                operationId.value(),
                localPath.generic_string(),
                remotePath.generic_string(),
                allowOverwrite,
                insertRefresh
            );
        }
    );
}

void SftpFileEngine::remove(std::vector<std::filesystem::path> const& paths, std::function<void(bool)> onComplete)
{
    Log::info("Requesting to remove {} items", paths.size());

    std::vector<std::string> transformedPaths;
    transformedPaths.resize(paths.size());
    std::transform(
        paths.begin(),
        paths.end(),
        transformedPaths.begin(),
        [](std::filesystem::path const& p)
        {
            return p.generic_string();
        }
    );

    lazyOpen(
        [this,
            onComplete = std::move(onComplete),
            transformedPaths = std::move(transformedPaths)](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot add upload, no channel");
                onComplete(false);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::deleteFiles", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to delete files: {}", val["error"].as<std::string>());
                        onComplete(false);
                        return;
                    }
                    onComplete(true);
                },
                channelId.value().value(),
                transformedPaths
            );
        }
    );
}
void SftpFileEngine::rename(
    std::filesystem::path const& oldPath,
    std::filesystem::path const& newPath,
    std::function<void(bool)> onComplete
)
{
    Log::info("Requesting to rename file: {} -> {}", oldPath.generic_string(), newPath.generic_string());

    lazyOpen(
        [this, oldPath, newPath, onComplete = std::move(onComplete)](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot rename file, no channel");
                onComplete(false);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::rename", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to rename file: {}", val["error"].as<std::string>());
                        onComplete(false);
                        return;
                    }
                    onComplete(true);
                },
                channelId.value().value(),
                oldPath.generic_string(),
                newPath.generic_string()
            );
        }
    );
}

void SftpFileEngine::stat(
    std::filesystem::path const& path,
    std::function<void(std::optional<std::pair<bool /*exists*/, SharedData::DirectoryEntry>> const&)> onComplete
)
{
    Log::info("Requesting info of file: {}", path.generic_string());

    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId)
        {
            if (!channelId)
            {
                Log::error("Cannot stat file, no channel");
                onComplete(std::nullopt);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::stat", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log("stat val", val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to rename file: {}", val["error"].as<std::string>());
                        onComplete(std::nullopt);
                        return;
                    }
                    if (!val.hasOwnProperty("stat") || val["stat"].isNull() || val["stat"].isUndefined())
                    {
                        onComplete(std::pair<bool /*exists*/, SharedData::DirectoryEntry>{false, {}});
                        return;
                    }
                    onComplete(
                        std::pair<bool /*exists*/, SharedData::DirectoryEntry>{
                            true, nlohmann::json::parse(Nui::JSON::stringify(val))["stat"]
                        }
                    );
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}