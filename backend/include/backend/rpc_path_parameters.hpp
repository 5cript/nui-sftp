#pragma once

#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <utility/error_message.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace RpcHelper
{
    /**
     * @brief A remote path from the frontend, or the message to reply with when it is not a valid absolute path.
     */
    std::expected<Paths::RemotePath, std::string> parseRemotePath(std::string const& path);

    /**
     * @brief The remote paths from the frontend, or the message about the first one that is not a valid absolute path.
     */
    std::expected<std::vector<Paths::RemotePath>, std::string> parseRemotePaths(std::vector<std::string> const& paths);

    /**
     * @brief A local path from the frontend, or the message to reply with when it is not a valid absolute path.
     */
    std::expected<Paths::LocalPath, std::string> parseLocalPath(std::string const& path);

    /**
     * @brief The local paths from the frontend, or the message about the first one that is not a valid absolute path.
     */
    std::expected<std::vector<Paths::LocalPath>, std::string> parseLocalPaths(std::vector<std::string> const& paths);

    /**
     * @brief A JSON request from the frontend, or the message to reply with when it does not parse. Paths are
     *        validated while parsing.
     *
     * @param what Names the request in the message, e.g. "bulk download".
     */
    template <typename RequestT>
    std::expected<RequestT, std::string> parseRequest(nlohmann::json const& json, std::string_view what)
    {
        try
        {
            return json.get<RequestT>();
        }
        catch (std::exception const& exception)
        {
            return std::unexpected(fmt::format("Invalid {}: {}", what, Utility::errorMessage(exception)));
        }
    }
}
