#include <backend/rpc_filesystem.hpp>
#include <log/log.hpp>
#include <shared_data/directory_entry.hpp>

#include <nui/backend/filesystem/special_paths.hpp>

#include <fstream>

RpcFilesystem::RpcFilesystem(
    boost::asio::any_io_executor executor,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    Persistence::LocalFilesystemOptions options
)
    : RpcHelper::StrandRpc{executor, wnd, hub}
    , options_{std::move(options)}
{
    registerRemove();
    registerRemoveMultiple();
    registerRename();
    registerListFiles();
    registerCreateFile();
    registerCreateDirectory();
    registerProperties();
    registerGetHome();
    registerDoesExist();
    registerWriteFile();
}

void RpcFilesystem::registerRemove()
{
    on("RpcFilesystem::remove")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!options_.preventDeletion)
                {
                    Log::warn("RpcFilesystem::remove called but deletion is prevented by configuration.");
                    return reply.error("File deletion is prevented by configuration.");
                }

                Log::info("RpcFilesystem::remove called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::remove", parameters}.hasValueDeep("path"))
                {
                    Log::error("RpcFilesystem::remove missing required parameter 'path'.");
                    return reply.error("Missing required parameter 'path'.");
                }

                const bool recursive = parameters.value("recursive", false);
                const auto path = parameters["path"].get<std::string>();

                if (!recursive)
                {
                    std::error_code ec;
                    if (std::filesystem::remove(path, ec))
                    {
                        Log::info("Successfully removed file: {}", path);
                        return reply({{"success", true}});
                    }

                    Log::error("Failed to remove file: {}: {}", path, ec.message());
                    return reply.error(ec.message());
                }

                std::error_code ec;
                const auto filesRemoved = std::filesystem::remove_all(path, ec);
                if (ec)
                {
                    Log::error("Failed to recursively remove path: {}: {}", path, ec.message());
                    return reply.error(ec.message());
                }
                Log::info(
                    "Successfully recursively removed path: {} ({} files/directories removed)", path, filesRemoved
                );
                return reply({{"success", true}, {"removedCount", filesRemoved}});
            }
        );
}

void RpcFilesystem::registerRemoveMultiple()
{
    on("RpcFilesystem::removeSome")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (options_.preventDeletion)
                {
                    Log::warn("RpcFilesystem::remove called but deletion is prevented by configuration.");
                    return reply.error("File deletion is prevented by configuration.");
                }

                Log::info("RpcFilesystem::removeSome called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::removeSome", parameters}.hasValueDeep(
                        "paths"
                    ))
                {
                    Log::error("RpcFilesystem::removeSome missing required parameter 'paths'.");
                    return reply.error("Missing required parameter 'paths'.");
                }

                const bool recursive = parameters.value("recursive", false);
                const auto paths = parameters["paths"].get<std::vector<std::string>>();
                size_t removedCount = 0;
                for (const auto& path : paths)
                {
                    std::error_code ec;
                    if (recursive)
                    {
                        removedCount += std::filesystem::remove_all(path, ec);
                    }
                    else
                    {
                        if (std::filesystem::remove(path, ec))
                            ++removedCount;
                    }

                    if (ec)
                    {
                        Log::error("Failed to remove path '{}': {}", path, ec.message());
                        return reply.error(fmt::format("Failed to remove path '{}': {}", path, ec.message()));
                    }
                }
                Log::info("Successfully removed all specified paths.");
                return reply({{"success", true}, {"removedCount", removedCount}});
            }
        );
}

void RpcFilesystem::registerRename()
{
    on("RpcFilesystem::rename")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (options_.preventRename)
                {
                    Log::warn("RpcFilesystem::rename called but renaming is prevented by configuration.");
                    return reply.error("File renaming is prevented by configuration.");
                }

                Log::info("RpcFilesystem::rename called with parameters: {}", parameters.dump(4));

                auto view = RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::rename", parameters};
                if (!view.hasValueDeep("oldPath") || !view.hasValueDeep("newPath"))
                {
                    Log::error("RpcFilesystem::rename missing required parameters 'oldPath' and/or 'newPath'.");
                    return reply.error("Missing required parameters 'oldPath' and/or 'newPath'.");
                }

                const auto oldPath = parameters["oldPath"].get<std::string>();
                const auto newPath = parameters["newPath"].get<std::string>();

                std::error_code ec;
                std::filesystem::rename(oldPath, newPath, ec);
                if (ec)
                {
                    Log::error("Failed to rename from '{}' to '{}': {}", oldPath, newPath, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully renamed from '{}' to '{}'", oldPath, newPath);
                return reply({{"success", true}});
            }
        );
}

void RpcFilesystem::registerListFiles()
{
    on("RpcFilesystem::listFiles")
        .perform(
            [](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                Log::info("RpcFilesystem::listFiles called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::listFiles", parameters}.hasValueDeep("path"))
                {
                    Log::error("RpcFilesystem::listFiles missing required parameter 'path'.");
                    return reply.error("Missing required parameter 'path'.");
                }

                auto directoryPath = std::filesystem::path{parameters["path"].get<std::string>()};
                const auto fileNameOnly = parameters.value("fileNameOnly", false);
                nlohmann::json fileList = nlohmann::json::array();

#ifdef _WIN32
                if (directoryPath == directoryPath.root_path())
                    directoryPath = std::filesystem::path(directoryPath.string() + "/");
#endif
                Log::info("Listing files in directory: {}", directoryPath.generic_string());

                std::error_code ec;
                auto iter = std::filesystem::directory_iterator(directoryPath, ec);
                if (ec)
                {
                    Log::error(
                        "Failed to list files in directory '{}': {}", directoryPath.generic_string(), ec.message()
                    );
                    return reply.error(ec.message());
                }
                for (const auto& entry : iter)
                {
                    fileList.push_back(
                        nlohmann::json(
                            SharedData::DirectoryEntry{
                                .path = [&entry, fileNameOnly]() -> std::string
                                {
                                    if (fileNameOnly)
                                    {
                                        const auto u8String = entry.path().filename().generic_u8string();
                                        return {u8String.begin(), u8String.end()};
                                    }
                                    const auto u8String = entry.path().generic_u8string();
                                    return {u8String.begin(), u8String.end()};
                                }(),
                                .type = SharedData::fileTypeFromStdFilesystemType(entry.symlink_status().type()),
                                .size = entry.is_regular_file() ? entry.file_size() : 0,
                                .resolvedType = SharedData::fileTypeFromStdFilesystemType(entry.status().type()),
                            }
                        )
                    );
                }

                Log::info("Successfully listed files in directory '{}'", directoryPath.generic_string());
                return reply({{"success", true}, {"files", fileList}});
            }
        );
}
void RpcFilesystem::registerCreateFile()
{
    on("RpcFilesystem::createFile")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (options_.preventCreateFile)
                {
                    Log::warn("RpcFilesystem::createFile called but file creation is prevented by configuration.");
                    return reply.error("File creation is prevented by configuration.");
                }

                Log::info("RpcFilesystem::createFile called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::createFile", parameters}.hasValueDeep(
                        "filePath"
                    ))
                {
                    Log::error("RpcFilesystem::createFile missing required parameter 'filePath'.");
                    return reply.error("Missing required parameter 'filePath'.");
                }

                const auto filePath = parameters["filePath"].get<std::string>();

                std::error_code ec;
                std::ofstream fileStream(filePath, std::ios_base::noreplace | std::ios_base::binary);
                if (!fileStream)
                {
                    Log::error("Failed to create file '{}': {}", filePath, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully created file '{}'", filePath);
                return reply({{"success", true}});
            }
        );
}
void RpcFilesystem::registerCreateDirectory()
{
    on("RpcFilesystem::createDirectory")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (options_.preventCreateDirectory)
                {
                    Log::warn(
                        "RpcFilesystem::createDirectory called but directory creation is prevented by configuration."
                    );
                    return reply.error("Directory creation is prevented by configuration.");
                }

                Log::info("RpcFilesystem::createDirectory called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::createDirectory", parameters}.hasValueDeep(
                        "path"
                    ))
                {
                    Log::error("RpcFilesystem::createDirectory missing required parameter 'path'.");
                    return reply.error("Missing required parameter 'path'.");
                }

                const auto directoryPath = parameters["path"].get<std::string>();

                std::error_code ec;
                std::filesystem::create_directory(directoryPath, ec);
                if (ec)
                {
                    Log::error("Failed to create directory '{}': {}", directoryPath, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully created directory '{}'", directoryPath);
                return reply({{"success", true}});
            }
        );
}
void RpcFilesystem::registerProperties()
{
    on("RpcFilesystem::properties")
        .perform(
            [](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                Log::info("RpcFilesystem::properties called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::properties", parameters}.hasValueDeep(
                        "path"
                    ))
                {
                    Log::error("RpcFilesystem::properties missing required parameter 'path'.");
                    return reply.error("Missing required parameter 'path'.");
                }

                const auto path = parameters["path"].get<std::string>();
                std::error_code ec;
                const auto status = std::filesystem::status(path, ec);
                if (ec)
                {
                    Log::error("Failed to get properties for path '{}': {}", path, ec.message());
                    return reply.error(ec.message());
                }

                nlohmann::json properties = {
                    {"type", static_cast<int>(status.type())},
                    {"size", std::filesystem::is_regular_file(status) ? std::filesystem::file_size(path, ec) : 0},
                };

                Log::info("Successfully retrieved properties for path '{}'", path);
                return reply({{"success", true}, {"properties", properties}});
            }
        );
}

void RpcFilesystem::registerGetHome()
{
    on("RpcFilesystem::getHome")
        .perform(
            [options = this->options_](RpcHelper::RpcOnce&& reply)
            {
                Log::info("RpcFilesystem::getHome called.");

                if (options.homeOverride.has_value())
                {
                    std::filesystem::path homePath = *options.homeOverride;
                    Log::info("Returning overridden home directory path: {}", homePath.generic_string());
                    return reply({{"success", true}, {"path", homePath.generic_string()}});
                }

                auto home = Nui::resolvePath("%userprofile%");

                Log::info("Returning initial directory path: {}", home.generic_string());
                return reply({{"success", true}, {"path", home.generic_string()}});
            }
        );
}

void RpcFilesystem::registerDoesExist()
{
    on("RpcFilesystem::exists")
        .perform(
            [](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::exists", parameters}.hasValueDeep("path"))
                {
                    Log::error("RpcFilesystem::exists missing required parameter 'path'.");
                    return reply.error("Missing required parameter 'path'.");
                }

                const auto path = parameters["path"].get<std::string>();
                std::error_code ec;
                const auto status = std::filesystem::exists(path, ec);
                if (ec)
                {
                    Log::error("Failed to get properties for path '{}': {}", path, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully retrieved existence for path '{}'", path);
                return reply({{"success", true}, {"exists", status}});
            }
        );
}

void RpcFilesystem::registerWriteFile()
{
    on("RpcFilesystem::writeFile")
        .perform(
            [](RpcHelper::RpcOnce&& reply, std::string const& filePath, std::string const& content)
            {
                Log::info("RpcFilesystem::writeFile called for file: {}", filePath);

                std::error_code ec;
                std::ofstream fileStream(filePath, std::ios_base::binary);
                if (!fileStream)
                {
                    Log::error("Failed to open file '{}' for writing: {}", filePath, ec.message());
                    return reply.error(ec.message());
                }

                fileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!fileStream)
                {
                    Log::error("Failed to write to file '{}': {}", filePath, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully wrote to file '{}'", filePath);
                return reply({{"success", true}});
            }
        );
}