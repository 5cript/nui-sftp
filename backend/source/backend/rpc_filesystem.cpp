#include <backend/rpc_filesystem.hpp>
#include <log/log.hpp>
#include <shared_data/directory_entry.hpp>
#include <utility/path_utf.hpp>

#include <nui/backend/filesystem/special_paths.hpp>

#include <fstream>

#ifndef _WIN32
#    include <sys/stat.h>
#    include <pwd.h>
#    include <grp.h>
#endif
#ifdef __linux__
#    include <fcntl.h>
#endif

#include <unordered_map>

namespace
{
#ifndef _WIN32
    /**
     *  @brief Per-call uid/gid → name cache, so a directory full of files owned by the same user
     *         only triggers one getpwuid_r / getgrgid_r call instead of one per entry.
     */
    struct UserGroupCache
    {
        std::unordered_map<std::uint32_t, std::string> users;
        std::unordered_map<std::uint32_t, std::string> groups;

        std::string const& user(std::uint32_t uid)
        {
            const auto found = users.find(uid);
            if (found != users.end())
                return found->second;
            char buffer[1024];
            struct passwd pwd{};
            struct passwd* result = nullptr;
            std::string name;
            if (::getpwuid_r(uid, &pwd, buffer, sizeof(buffer), &result) == 0 && result)
                name = result->pw_name;
            return users.emplace(uid, std::move(name)).first->second;
        }

        std::string const& group(std::uint32_t gid)
        {
            const auto found = groups.find(gid);
            if (found != groups.end())
                return found->second;
            char buffer[1024];
            struct group grp{};
            struct group* result = nullptr;
            std::string name;
            if (::getgrgid_r(gid, &grp, buffer, sizeof(buffer), &result) == 0 && result)
                name = result->gr_name;
            return groups.emplace(gid, std::move(name)).first->second;
        }
    };

    /**
     *  @brief Lean stat fill for directory listing — drops atime (only used by the Properties dialog,
     *         which now fetches its own full entry on demand).
     */
    void fillFromPosixStatLean(SharedData::DirectoryEntry& entry, struct stat const& st, UserGroupCache& cache)
    {
        entry.uid = static_cast<std::uint32_t>(st.st_uid);
        entry.gid = static_cast<std::uint32_t>(st.st_gid);
        entry.size = static_cast<std::uint64_t>(st.st_size);
        entry.mtime = static_cast<std::uint64_t>(st.st_mtim.tv_sec);
        entry.mtimeNsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
        entry.owner = cache.user(entry.uid);
        entry.group = cache.group(entry.gid);
    }

    /**
     *  @brief Full stat fill for the Properties dialog — also captures atime.
     */
    void fillFromPosixStat(SharedData::DirectoryEntry& entry, struct stat const& st)
    {
        entry.uid = static_cast<std::uint32_t>(st.st_uid);
        entry.gid = static_cast<std::uint32_t>(st.st_gid);
        entry.size = static_cast<std::uint64_t>(st.st_size);
        entry.atime = static_cast<std::uint64_t>(st.st_atim.tv_sec);
        entry.atimeNsec = static_cast<std::uint32_t>(st.st_atim.tv_nsec);
        entry.mtime = static_cast<std::uint64_t>(st.st_mtim.tv_sec);
        entry.mtimeNsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);

        char pwdBuf[1024];
        struct passwd pwd{};
        struct passwd* pwdResult = nullptr;
        if (::getpwuid_r(st.st_uid, &pwd, pwdBuf, sizeof(pwdBuf), &pwdResult) == 0 && pwdResult)
            entry.owner = pwdResult->pw_name;

        char grpBuf[1024];
        struct group grp{};
        struct group* grpResult = nullptr;
        if (::getgrgid_r(st.st_gid, &grp, grpBuf, sizeof(grpBuf), &grpResult) == 0 && grpResult)
            entry.group = grpResult->gr_name;
    }
#endif

#ifdef __linux__
    void fillBirthTime(SharedData::DirectoryEntry& entry, char const* path, int statxFlags)
    {
        struct statx stx{};
        if (::statx(AT_FDCWD, path, statxFlags, STATX_BTIME, &stx) == 0 && (stx.stx_mask & STATX_BTIME))
        {
            entry.createTime = static_cast<std::uint64_t>(stx.stx_btime.tv_sec);
            entry.createTimeNsec = static_cast<std::uint32_t>(stx.stx_btime.tv_nsec);
        }
    }
#endif
}

RpcFilesystem::RpcFilesystem(
    boost::asio::any_io_executor executor,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    Persistence::LocalFilesystemOptions options,
    Opener& opener
)
    : RpcHelper::StrandRpc{executor, wnd, hub}
    , options_{std::move(options)}
    , opener_{&opener}
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
    registerDoesExistBatch();
    registerWriteFile();
    registerOpen();
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
                const auto pathUtf8 = parameters["path"].get<std::string>();
                const auto path = Utility::pathFromUtf8(pathUtf8);

                if (!recursive)
                {
                    std::error_code ec;
                    if (std::filesystem::remove(path, ec))
                    {
                        Log::info("Successfully removed file: {}", pathUtf8);
                        return reply({{"success", true}});
                    }

                    Log::error("Failed to remove file: {}: {}", pathUtf8, ec.message());
                    return reply.error(ec.message());
                }

                std::error_code ec;
                const auto filesRemoved = std::filesystem::remove_all(path, ec);
                if (ec)
                {
                    Log::error("Failed to recursively remove path: {}: {}", pathUtf8, ec.message());
                    return reply.error(ec.message());
                }
                Log::info(
                    "Successfully recursively removed path: {} ({} files/directories removed)",
                    pathUtf8,
                    filesRemoved
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

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::removeSome", parameters}.hasValueDeep(
                        "paths"
                    ))
                {
                    Log::error("RpcFilesystem::removeSome missing required parameter 'paths'.");
                    return reply.error("Missing required parameter 'paths'.");
                }

                const bool recursive = parameters.value("recursive", false);
                const auto pathsUtf8 = parameters["paths"].get<std::vector<std::string>>();
                size_t removedCount = 0;
                for (const auto& pathUtf8 : pathsUtf8)
                {
                    const auto path = Utility::pathFromUtf8(pathUtf8);
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
                        Log::error("Failed to remove path '{}': {}", pathUtf8, ec.message());
                        return reply.error(fmt::format("Failed to remove path '{}': {}", pathUtf8, ec.message()));
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

                const auto oldPathUtf8 = parameters["oldPath"].get<std::string>();
                const auto newPathUtf8 = parameters["newPath"].get<std::string>();
                const auto oldPath = Utility::pathFromUtf8(oldPathUtf8);
                const auto newPath = Utility::pathFromUtf8(newPathUtf8);

                std::error_code ec;
                std::filesystem::rename(oldPath, newPath, ec);
                if (ec)
                {
                    Log::error("Failed to rename from '{}' to '{}': {}", oldPathUtf8, newPathUtf8, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully renamed from '{}' to '{}'", oldPathUtf8, newPathUtf8);
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

                auto directoryPath = Utility::pathFromUtf8(parameters["path"].get<std::string>());
                const auto fileNameOnly = parameters.value("fileNameOnly", false);
                nlohmann::json fileList = nlohmann::json::array();

#ifdef _WIN32
                if (directoryPath == directoryPath.root_path())
                    directoryPath += "/";
#endif
                Log::info("Listing files in directory: {}", Utility::pathToUtf8Generic(directoryPath));

                std::error_code ec;
                auto iter = std::filesystem::directory_iterator(directoryPath, ec);
                if (ec)
                {
                    Log::error(
                        "Failed to list files in directory '{}': {}", Utility::pathToUtf8Generic(directoryPath), ec.message()
                    );
                    return reply.error(ec.message());
                }

#ifndef _WIN32
                UserGroupCache cache;
#endif
                for (const auto& entry : iter)
                {
                    const auto entryPath = entry.path();

                    SharedData::DirectoryEntry dirEntry{};
                    dirEntry.path =
                        Utility::pathToUtf8Generic(fileNameOnly ? entryPath.filename() : entryPath);
                    dirEntry.type = SharedData::fileTypeFromStdFilesystemType(entry.symlink_status().type());
                    dirEntry.permissions = entry.symlink_status().permissions();

#ifndef _WIN32
                    struct stat lstSt{};
                    if (::lstat(entryPath.c_str(), &lstSt) == 0)
                        fillFromPosixStatLean(dirEntry, lstSt, cache);
#endif

                    if (dirEntry.type == SharedData::FileType::Symlink)
                    {
                        std::error_code linkEc;
                        auto linkTarget = std::filesystem::read_symlink(entryPath, linkEc);
                        if (!linkEc)
                            dirEntry.linkTarget = std::move(linkTarget);

                        auto resolved = std::make_shared<SharedData::DirectoryEntry>();
                        std::error_code pathEc;
                        auto canonical = std::filesystem::canonical(entryPath, pathEc);
                        if (!pathEc)
                            resolved->path = std::move(canonical);
                        else if (dirEntry.linkTarget.has_value())
                            resolved->path = *dirEntry.linkTarget;

                        resolved->type = SharedData::fileTypeFromStdFilesystemType(entry.status().type());
                        resolved->permissions = entry.status().permissions();
#ifndef _WIN32
                        struct stat stSt{};
                        if (::stat(entryPath.c_str(), &stSt) == 0)
                            fillFromPosixStatLean(*resolved, stSt, cache);
#endif
                        dirEntry.resolvedTarget = std::move(resolved);
                    }

                    fileList.push_back(nlohmann::json(dirEntry));
                }

                Log::info("Successfully listed files in directory '{}'", Utility::pathToUtf8Generic(directoryPath));
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

                const auto filePathUtf8 = parameters["filePath"].get<std::string>();
                const auto filePath = Utility::pathFromUtf8(filePathUtf8);

                std::error_code ec;
                std::ofstream fileStream(filePath, std::ios_base::noreplace | std::ios_base::binary);
                if (!fileStream)
                {
                    Log::error("Failed to create file '{}': {}", filePathUtf8, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully created file '{}'", filePathUtf8);
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

                const auto directoryPathUtf8 = parameters["path"].get<std::string>();
                const auto directoryPath = Utility::pathFromUtf8(directoryPathUtf8);

                std::error_code ec;
                std::filesystem::create_directory(directoryPath, ec);
                if (ec)
                {
                    Log::error("Failed to create directory '{}': {}", directoryPathUtf8, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully created directory '{}'", directoryPathUtf8);
                return reply({{"success", true}});
            }
        );
}
void RpcFilesystem::registerProperties()
{
    // Returns a fully-populated DirectoryEntry for a single path. listFiles intentionally
    // omits expensive fields (atime, birthtime, acl, second stat) so the dialog fetches
    // them on demand here.
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

                const auto path = Utility::pathFromUtf8(parameters["path"].get<std::string>());

                SharedData::DirectoryEntry entry{};
                entry.path = Utility::pathToUtf8Generic(path);

                std::error_code symEc;
                const auto symStatus = std::filesystem::symlink_status(path, symEc);
                if (symEc)
                {
                    Log::error("Failed to get properties for path '{}': {}", Utility::pathToUtf8Generic(path), symEc.message());
                    return reply.error(symEc.message());
                }
                entry.type = SharedData::fileTypeFromStdFilesystemType(symStatus.type());
                entry.permissions = symStatus.permissions();

#ifndef _WIN32
                struct stat lstSt{};
                if (::lstat(path.c_str(), &lstSt) == 0)
                    fillFromPosixStat(entry, lstSt);
#    ifdef __linux__
                fillBirthTime(entry, path.c_str(), AT_SYMLINK_NOFOLLOW);
#    endif
#endif

                if (entry.type == SharedData::FileType::Symlink)
                {
                    std::error_code linkEc;
                    auto linkTarget = std::filesystem::read_symlink(path, linkEc);
                    if (!linkEc)
                        entry.linkTarget = std::move(linkTarget);

                    auto resolved = std::make_shared<SharedData::DirectoryEntry>();
                    std::error_code pathEc;
                    auto canonical = std::filesystem::canonical(path, pathEc);
                    if (!pathEc)
                        resolved->path = std::move(canonical);
                    else if (entry.linkTarget.has_value())
                        resolved->path = *entry.linkTarget;

                    std::error_code statusEc;
                    const auto resolvedStatus = std::filesystem::status(path, statusEc);
                    if (!statusEc)
                    {
                        resolved->type = SharedData::fileTypeFromStdFilesystemType(resolvedStatus.type());
                        resolved->permissions = resolvedStatus.permissions();
                    }

#ifndef _WIN32
                    struct stat stSt{};
                    if (::stat(path.c_str(), &stSt) == 0)
                        fillFromPosixStat(*resolved, stSt);
#    ifdef __linux__
                    fillBirthTime(*resolved, path.c_str(), 0 /*follow symlinks*/);
#    endif
#endif
                    entry.resolvedTarget = std::move(resolved);
                }

                Log::info("Successfully retrieved properties for path '{}'", Utility::pathToUtf8Generic(path));
                return reply({{"success", true}, {"entry", nlohmann::json(entry)}});
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
                    const auto homePathUtf8 = Utility::pathToUtf8Generic(homePath);
                    Log::info("Returning overridden home directory path: {}", homePathUtf8);
                    return reply({{"success", true}, {"path", homePathUtf8}});
                }

                auto home = Nui::resolvePath("%userprofile%");

                const auto homeUtf8 = Utility::pathToUtf8Generic(home);
                Log::info("Returning initial directory path: {}", homeUtf8);
                return reply({{"success", true}, {"path", homeUtf8}});
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

                const auto pathUtf8 = parameters["path"].get<std::string>();
                const auto path = Utility::pathFromUtf8(pathUtf8);
                std::error_code ec;
                const auto status = std::filesystem::exists(path, ec);
                if (ec)
                {
                    Log::error("Failed to get properties for path '{}': {}", pathUtf8, ec.message());
                    return reply.error(ec.message());
                }

                Log::info("Successfully retrieved existence for path '{}'", pathUtf8);
                return reply({{"success", true}, {"exists", status}});
            }
        );
}

void RpcFilesystem::registerDoesExistBatch()
{
    // Batched form of RpcFilesystem::exists.  Lets the frontend ask about a
    // whole transfer's worth of destination paths in a single round-trip
    // instead of N — eliminating the per-file network latency that made
    // multi-file drag/drop downloads feel slow.
    on("RpcFilesystem::existsBatch")
        .perform(
            [](RpcHelper::RpcOnce&& reply, std::vector<std::string> const& paths)
            {
                std::vector<bool> results;
                results.reserve(paths.size());
                for (auto const& pathUtf8 : paths)
                {
                    std::error_code ec;
                    const auto status = std::filesystem::exists(Utility::pathFromUtf8(pathUtf8), ec);
                    if (ec)
                    {
                        Log::warn(
                            "RpcFilesystem::existsBatch: stat failed for '{}': {} (treating as not-exists)",
                            pathUtf8,
                            ec.message()
                        );
                        results.push_back(false);
                        continue;
                    }
                    results.push_back(status);
                }
                Log::info("RpcFilesystem::existsBatch: probed {} paths", paths.size());
                return reply({{"success", true}, {"exists", results}});
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

void RpcFilesystem::registerOpen()
{
    on("RpcFilesystem::open")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, std::string const& filePath, bool openWith)
            {
                Log::info("RpcFilesystem::open called for file: {}", filePath);
                auto result = opener_->openFile(Utility::pathFromUtf8(filePath), openWith);
                if (!result)
                {
                    Log::error("Failed to open file '{}': {}", filePath, result.error());
                    return reply.error(result.error());
                }

                reply({{"success", true}});
            }
        );
}