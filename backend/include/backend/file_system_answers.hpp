#pragma once

#include <log/log.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system_operations.hpp>
#include <utility/user_directories.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>

/**
 * @brief How the RPCs on a file system channel answer once its call completed: each makes the answer of
 *        RpcHelper::answerOnCompletion, called with the reply and the result.
 */
namespace FileSystemAnswers
{
    /**
     * @brief The entries of the directory @p path.
     */
    inline auto listing(std::string path)
    {
        return [path = std::move(path)](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error(result.error().message);

            Log::info("Listed directory '{}', got {} entries", path, result->size());
            reply({{"entries", *result}});
        };
    }

    inline auto homeDirectory()
    {
        return [](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error("Failed to get home directory: " + result.error().toString());

            const auto& home = result->value();
            Log::info("Remote home directory is '{}'", home);
            reply({{"path", home}});
        };
    }

    /**
     * @brief The user directories found in @p home.
     */
    inline auto userDirectories(std::string home)
    {
        return [home = std::move(home)](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error("Failed to read user directories: " + result.error().toString());

            auto directories = nlohmann::json::array();
            for (auto const& directory : *result)
                directories.push_back({{"name", directory.name}, {"path", directory.path}});
            Log::info("Read {} user directories in '{}'", directories.size(), home);
            reply({{"directories", std::move(directories)}});
        };
    }

    inline auto createdDirectory(std::string path)
    {
        return [path = std::move(path)](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error(fmt::format("Failed to create directory '{}': {}", path, result.error().toString()));

            Log::info("Created directory '{}'", path);
            reply({{"success", true}});
        };
    }

    inline auto createdFile(std::string path)
    {
        return [path = std::move(path)](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error("Failed to create file: " + result.error().message);

            Log::info("Created file '{}'", path);
            reply({{"success", true}});
        };
    }

    /**
     * @brief The directories that are not empty.
     */
    inline auto preDeleteChecks()
    {
        return [](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error("Failed to perform pre-delete checks: " + result.error().toString());
            reply({{"success", true}, {"nonEmptyDirectories", *result}});
        };
    }

    inline auto deletedFiles()
    {
        return [](auto const& reply, auto const& result)
        {
            if (!result.has_value())
                return reply.error("Failed to delete files: " + result.error().toString());

            reply({{"success", true}});
        };
    }

    /**
     * @brief A destination that exists answers with the key backend.rename.targetExists.
     */
    inline auto renamed()
    {
        return [](auto const& reply, auto const& result)
        {
            if (!result.has_value())
            {
                if (result.error().kind == RemoteIo::RemoteErrorKind::AlreadyExists)
                    return reply.error("backend.rename.targetExists");
                return reply.error("Failed to rename file: " + result.error().toString());
            }
            reply({{"success", true}});
        };
    }

    /**
     * @brief A missing file answers with no stat.
     */
    inline auto stat()
    {
        return [](auto const& reply, auto const& result)
        {
            if (!result.has_value())
            {
                if (result.error().kind == RemoteIo::RemoteErrorKind::NoSuchFile)
                    return reply({{"success", true}, {"stat", nullptr}});
                return reply.error("Failed to stat file: " + result.error().toString());
            }
            reply({{"success", true}, {"stat", *result}});
        };
    }
}
