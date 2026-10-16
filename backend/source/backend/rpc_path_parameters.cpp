#include <backend/rpc_path_parameters.hpp>

#include <paths/path_error.hpp>

#include <utility>

namespace RpcHelper
{
    namespace
    {
        template <typename PathT, typename ParseT>
        std::expected<std::vector<PathT>, std::string> parseAll(std::vector<std::string> const& paths, ParseT parse)
        {
            std::vector<PathT> parsed;
            parsed.reserve(paths.size());
            for (auto const& path : paths)
            {
                auto one = parse(path);
                if (!one)
                    return std::unexpected(std::move(one).error());
                parsed.push_back(*std::move(one));
            }
            return parsed;
        }
    }

    std::expected<Paths::RemotePath, std::string> parseRemotePath(std::string const& path)
    {
        return Paths::RemotePath::make(path).transform_error(
            [](Paths::PathError const& error)
            {
                return fmt::format("Invalid remote path: {}", error.toString());
            }
        );
    }

    std::expected<std::vector<Paths::RemotePath>, std::string> parseRemotePaths(std::vector<std::string> const& paths)
    {
        return parseAll<Paths::RemotePath>(paths, parseRemotePath);
    }

    std::expected<Paths::LocalPath, std::string> parseLocalPath(std::string const& path)
    {
        return Paths::LocalPath::fromUtf8(path).transform_error(
            [](Paths::PathError const& error)
            {
                return fmt::format("Invalid local path: {}", error.toString());
            }
        );
    }

    std::expected<std::vector<Paths::LocalPath>, std::string> parseLocalPaths(std::vector<std::string> const& paths)
    {
        return parseAll<Paths::LocalPath>(paths, parseLocalPath);
    }
}
