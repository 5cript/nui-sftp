#include <backend/rpc_filesystem.hpp>
#include <utility/link_safe_remove.hpp>
#include <log/log.hpp>
#include <shared_data/directory_entry.hpp>
#include <paths/native.hpp>
#include <utility/localized_message.hpp>
#include <utility/rename.hpp>
#include <utility/rename_target.hpp>
#include <backend/local_delete_guard.hpp>
#include <backend/existence_check.hpp>

#include <nui/backend/filesystem/special_paths.hpp>

#include <fmt/ranges.h>

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
    /**
     * @brief The native form of the absolute local path @p utf8, or why it is refused.
     */
    std::expected<std::filesystem::path, std::string> localPathParameter(std::string const& utf8)
    {
        return Paths::LocalPath::fromUtf8(utf8)
            .transform(
                [](Paths::LocalPath const& path)
                {
                    return path.native();
                }
            )
            .transform_error(
                [](Paths::PathError const& error)
                {
                    return error.toString();
                }
            );
    }

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
                name = Paths::sanitizedUtf8(result->pw_name);
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
                name = Paths::sanitizedUtf8(result->gr_name);
            return groups.emplace(gid, std::move(name)).first->second;
        }
    };

    timespec modificationTime(struct stat const& st)
    {
#    ifdef __APPLE__
        return st.st_mtimespec;
#    else
        return st.st_mtim;
#    endif
    }

    timespec accessTime(struct stat const& st)
    {
#    ifdef __APPLE__
        return st.st_atimespec;
#    else
        return st.st_atim;
#    endif
    }

    /**
     *  @brief Lean stat fill for directory listing — drops atime (only used by the Properties dialog,
     *         which now fetches its own full entry on demand).
     */
    void fillFromPosixStatLean(SharedData::DirectoryEntry& entry, struct stat const& st, UserGroupCache& cache)
    {
        entry.uid = static_cast<std::uint32_t>(st.st_uid);
        entry.gid = static_cast<std::uint32_t>(st.st_gid);
        entry.size = static_cast<std::uint64_t>(st.st_size);
        const auto modified = modificationTime(st);
        entry.mtime = static_cast<std::uint64_t>(modified.tv_sec);
        entry.mtimeNsec = static_cast<std::uint32_t>(modified.tv_nsec);
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
        const auto accessed = accessTime(st);
        entry.atime = static_cast<std::uint64_t>(accessed.tv_sec);
        entry.atimeNsec = static_cast<std::uint32_t>(accessed.tv_nsec);
        const auto modified = modificationTime(st);
        entry.mtime = static_cast<std::uint64_t>(modified.tv_sec);
        entry.mtimeNsec = static_cast<std::uint32_t>(modified.tv_nsec);
#    ifdef __APPLE__
        entry.createTime = static_cast<std::uint64_t>(st.st_birthtimespec.tv_sec);
        entry.createTimeNsec = static_cast<std::uint32_t>(st.st_birthtimespec.tv_nsec);
#    endif

        char pwdBuf[1024];
        struct passwd pwd{};
        struct passwd* pwdResult = nullptr;
        if (::getpwuid_r(st.st_uid, &pwd, pwdBuf, sizeof(pwdBuf), &pwdResult) == 0 && pwdResult)
            entry.owner = Paths::sanitizedUtf8(pwdResult->pw_name);

        char grpBuf[1024];
        struct group grp{};
        struct group* grpResult = nullptr;
        if (::getgrgid_r(st.st_gid, &grp, grpBuf, sizeof(grpBuf), &grpResult) == 0 && grpResult)
            entry.group = Paths::sanitizedUtf8(grpResult->gr_name);
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
    Opener& opener,
    std::function<Persistence::RiskyFilePolicy()> riskyFilePolicy
)
    : RpcHelper::StrandRpc{executor, wnd, hub}
    , options_{std::move(options)}
    , opener_{&opener}
    , riskyFilePolicy_{std::move(riskyFilePolicy)}
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
    registerOpenInFileManager();
    registerOpenerCapabilities();
}

void RpcFilesystem::registerRemove()
{
    on("RpcFilesystem::remove")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (options_.preventDeletion)
                {
                    Log::warn("RpcFilesystem::remove called but deletion is prevented by configuration.");
                    return reply.error("backend.rpcFilesystem.deletionPrevented");
                }

                Log::info("RpcFilesystem::remove called with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "RpcFilesystem::remove", parameters}.hasValueDeep("path"))
                {
                    Log::error("RpcFilesystem::remove missing required parameter 'path'.");
                    return reply.error("Missing required parameter 'path'.");
                }

                const bool recursive = parameters.value("recursive", false);
                const auto pathUtf8 = parameters["path"].get<std::string>();
                const auto target = localDeleteTarget(pathUtf8, localHomeDirectories());
                if (!target)
                {
                    Log::error("RpcFilesystem::remove refused: {}", target.error());
                    return reply.error(Utility::localizedMessage("backend.rpcFilesystem.removeRefused", pathUtf8));
                }
                const auto path = target->native();

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
                const auto filesRemoved = Utility::removeAllNoFollow(path, ec);
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
                    return reply.error("backend.rpcFilesystem.deletionPrevented");
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
                const auto removal = removeLocalItems(pathsUtf8, localHomeDirectories());
                if (!removal)
                {
                    const auto path = target.native();
                    const auto& pathUtf8 = target.toUtf8();
                    std::error_code ec;
                    if (recursive)
                    {
                        removedCount += Utility::removeAllNoFollow(path, ec);
                    }
                    else
                    {
                        if (std::filesystem::remove(path, ec))
                            ++removedCount;
                    }

                    if (ec)
                    {
                        Log::error("Failed to remove path '{}': {}", pathUtf8, ec.message());
                        failures.push_back(fmt::format("{}: {}", pathUtf8, ec.message()));
                    }
                }
                if (!failures.empty())
                {
                    return reply.error(
                        Utility::localizedMessage(
                            "backend.rpcFilesystem.removeSomeFailed",
                            failures.size(),
                            fmt::format("{}", fmt::join(failures, "\n"))
                        )
                    );
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
                    return reply.error("backend.rpcFilesystem.renamePrevented");
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
                const auto oldPathParameter = localPathParameter(oldPathUtf8);
                const auto newPathParameter = localPathParameter(newPathUtf8);
                if (!oldPathParameter || !newPathParameter)
                    return reply.error(oldPathParameter ? newPathParameter.error() : oldPathParameter.error());
                auto const& oldPath = *oldPathParameter;
                auto const& newPath = *newPathParameter;
                const bool overwrite = parameters.contains("overwrite") && parameters["overwrite"].is_boolean() &&
                    parameters["overwrite"].get<bool>();

                if (!Utility::isRenameInPlace(oldPath, newPath))
                {
                    Log::error("RpcFilesystem::rename refused to move '{}' to '{}'.", oldPathUtf8, newPathUtf8);
                    return reply.error("backend.rename.invalidName");
                }

                if (const auto moved = Utility::moveIntoPlace(oldPath, newPath, overwrite); !moved)
                {
                    if (moved.error() == std::errc::file_exists)
                        return reply.error("backend.rename.targetExists");
                    Log::error(
                        "Failed to rename from '{}' to '{}': {}",
                        oldPathUtf8,
                        newPathUtf8,
                        Utility::errorMessage(moved.error())
                    );
                    return reply.error(Utility::errorMessage(moved.error()));
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

                // A drive "C:" is read as its root "C:/".
                const auto directoryParameter = localPathParameter(parameters["path"].get<std::string>());
                if (!directoryParameter)
                    return reply.error(directoryParameter.error());
                auto const& directoryPath = *directoryParameter;
                nlohmann::json fileList = nlohmann::json::array();

                Log::info("Listing files in directory: {}", Paths::nativeToUtf8(directoryPath));

                std::error_code ec;
                auto iter = std::filesystem::directory_iterator(directoryPath, ec);
                if (ec)
                {
                    Log::error(
                        "Failed to list files in directory '{}': {}", Paths::nativeToUtf8(directoryPath), ec.message()
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
                    auto name = Paths::convertNativeToUtf8(entryPath.filename());
                    dirEntry.name = std::move(name.utf8);
                    dirEntry.nameIsValidUtf8 = name.lossless;
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
                        const auto linkTarget = std::filesystem::read_symlink(entryPath, linkEc);
                        if (!linkEc)
                            dirEntry.linkTarget = Paths::nativeToUtf8(linkTarget);

                        auto resolved = std::make_shared<SharedData::DirectoryEntry>();
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

                Log::info("Successfully listed files in directory '{}'", Paths::nativeToUtf8(directoryPath));
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
                    return reply.error("backend.rpcFilesystem.createFilePrevented");
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
                const auto filePathParameter = localPathParameter(filePathUtf8);
                if (!filePathParameter)
                    return reply.error(filePathParameter.error());
                auto const& filePath = *filePathParameter;

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
                    return reply.error("backend.rpcFilesystem.createDirectoryPrevented");
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
                const auto directoryParameter = localPathParameter(directoryPathUtf8);
                if (!directoryParameter)
                    return reply.error(directoryParameter.error());
                auto const& directoryPath = *directoryParameter;

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

                const auto pathParameter = localPathParameter(parameters["path"].get<std::string>());
                if (!pathParameter)
                    return reply.error(pathParameter.error());
                auto const& path = *pathParameter;

                SharedData::DirectoryEntry entry{};
                auto name = Paths::convertNativeToUtf8(path.filename());
                entry.name = std::move(name.utf8);
                entry.nameIsValidUtf8 = name.lossless;
                std::string resolvedPath{};

                std::error_code symEc;
                const auto symStatus = std::filesystem::symlink_status(path, symEc);
                if (symEc)
                {
                    Log::error("Failed to get properties for path '{}': {}", Paths::nativeToUtf8(path), symEc.message());
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
                    const auto linkTarget = std::filesystem::read_symlink(path, linkEc);
                    if (!linkEc)
                        entry.linkTarget = Paths::nativeToUtf8(linkTarget);

                    auto resolved = std::make_shared<SharedData::DirectoryEntry>();
                    std::error_code pathEc;
                    const auto canonical = std::filesystem::canonical(path, pathEc);
                    if (!pathEc)
                    {
                        resolvedPath = Paths::nativeToUtf8(canonical);
                        resolved->name = Paths::nativeToUtf8(canonical.filename());
                    }
                    else if (entry.linkTarget.has_value())
                        resolvedPath = *entry.linkTarget;

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

                Log::info("Successfully retrieved properties for path '{}'", Paths::nativeToUtf8(path));
                // The paths go along for display, the entries carry names only.
                return reply(
                    {{"success", true},
                     {"entry", nlohmann::json(entry)},
                     {"path", Paths::nativeToUtf8(path)},
                     {"resolvedPath", resolvedPath}}
                );
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
                    const auto homePathUtf8 = Paths::nativeToUtf8(homePath);
                    Log::info("Returning overridden home directory path: {}", homePathUtf8);
                    return reply({{"success", true}, {"path", homePathUtf8}});
                }

                auto home = Nui::resolvePath("%userprofile%");

                const auto homeUtf8 = Paths::nativeToUtf8(home);
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
                const auto path = localPathParameter(pathUtf8);
                if (!path)
                    return reply.error(path.error());
                std::error_code ec;
                const auto status = std::filesystem::exists(*path, ec);
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
                auto results = localPathsExist(paths);
                if (!results)
                {
                    Log::error("RpcFilesystem::existsBatch: {}", results.error());
                    return reply.error(results.error());
                }
                Log::info("RpcFilesystem::existsBatch: probed {} paths", paths.size());
                return reply({{"success", true}, {"exists", *results}});
            }
        );
}

namespace
{
    /**
     * @brief Writes @p content to the UTF-8 path @p filePath and replies. Without @p replace an existing file is
     *        left alone and the reply's error is backend.rpcFilesystem.fileExists.
     */
    void writeFileAndReply(
        RpcHelper::RpcOnce&& reply,
        std::string const& filePath,
        std::string const& content,
        bool replace
    )
    {
        // The path is UTF-8, the narrow ofstream constructor reads the ANSI code page on Windows.
        const auto path = localPathParameter(filePath);
        if (!path)
            return reply.error(path.error());

        const auto mode = replace ? std::ios_base::binary : std::ios_base::binary | std::ios_base::noreplace;
        std::ofstream fileStream(*path, mode);
        if (!fileStream)
        {
            if (std::error_code statusError{}; !replace && std::filesystem::exists(std::filesystem::symlink_status(*path, statusError)))
                return reply.error(Utility::localizedMessage("backend.rpcFilesystem.fileExists", filePath));
            Log::error("Failed to open file '{}' for writing.", filePath);
            return reply.error(fmt::format("Cannot open '{}' for writing", filePath));
        }

        fileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!fileStream)
        {
            Log::error("Failed to write to file '{}'.", filePath);
            return reply.error(fmt::format("Cannot write '{}'", filePath));
        }

        Log::info("Successfully wrote to file '{}'", filePath);
        return reply({{"success", true}});
    }
}

void RpcFilesystem::registerWriteFile()
{
    on("RpcFilesystem::writeFile")
        .perform(
            [](RpcHelper::RpcOnce&& reply, std::string const& filePath, std::string const& content)
            {
                Log::info("RpcFilesystem::writeFile called for file: {}", filePath);
                writeFileAndReply(std::move(reply), filePath, content, /*replace=*/true);
            }
        );
    on("RpcFilesystem::writeNewFile")
        .perform(
            [](RpcHelper::RpcOnce&& reply, std::string const& filePath, std::string const& content)
            {
                Log::info("RpcFilesystem::writeNewFile called for file: {}", filePath);
                writeFileAndReply(std::move(reply), filePath, content, /*replace=*/false);
            }
        );
}

void RpcFilesystem::registerOpen()
{
    on("RpcFilesystem::open")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, std::string const& filePath, bool openWith, bool confirmed)
            {
                Log::info("RpcFilesystem::open called for file: {}", filePath);
                const auto path = localPathParameter(filePath);
                if (!path)
                    return reply.error(path.error());

                // Files that can run code follow the user's policy; the frontend asks and calls again confirmed.
                if (const auto risk = opener_->riskOf(*path); risk)
                {
                    const auto policy = riskyFilePolicy_ ? riskyFilePolicy_() : Persistence::RiskyFilePolicy::Warn;
                    if (policy == Persistence::RiskyFilePolicy::Never)
                    {
                        Log::warn("Refusing to open '{}': {}, opening such files is turned off.", filePath, *risk);
                        return reply.error(Utility::localizedMessage("backend.opener.risk.refused", filePath));
                    }
                    if (policy == Persistence::RiskyFilePolicy::Warn && !confirmed)
                        return reply({{"success", false}, {"needsConfirmation", true}, {"risk", *risk}});
                }

                auto result = opener_->openFile(*path, openWith);
                if (!result)
                {
                    Log::error("Failed to open file '{}': {}", filePath, result.error());
                    return reply.error(result.error());
                }

                reply({{"success", true}});
            }
        );
}

void RpcFilesystem::registerOpenerCapabilities()
{
    on("RpcFilesystem::openerCapabilities")
        .perform(
            [this](RpcHelper::RpcOnce&& reply)
            {
                const auto caps = opener_->capabilities();
                Log::info(
                    "RpcFilesystem::openerCapabilities: canOpenFile={}, canOpenInFileManager={}, reason='{}'",
                    caps.canOpenFile,
                    caps.canOpenInFileManager,
                    caps.reason);
                reply({{"success", true}, {"capabilities", nlohmann::json(caps)}});
            }
        );
}

void RpcFilesystem::registerOpenInFileManager()
{
    on("RpcFilesystem::openInFileManager")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, std::string const& filePath)
            {
                Log::info("RpcFilesystem::openInFileManager called for path: {}", filePath);
                const auto path = localPathParameter(filePath);
                if (!path)
                    return reply.error(path.error());
                auto result = opener_->openInFileManager(*path);
                if (!result)
                {
                    Log::error("Failed to open file manager for '{}': {}", filePath, result.error());
                    return reply.error(result.error());
                }

                reply({{"success", true}});
            }
        );
}