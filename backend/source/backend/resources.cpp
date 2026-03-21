#include <backend/resources.hpp>
#include <log/log.hpp>

namespace
{
    auto endsWithImpl(std::string_view pathString, std::string_view ending)
    {
        return pathString.size() >= ending.size() && pathString.substr(pathString.size() - ending.size()) == ending;
    };
}

bool isCanonical(std::filesystem::path const& path)
{
    // iterate segments and look for "." and ".."
    for (auto const& segment : path)
    {
        if (segment == "." || segment == "..")
            return false;
    }
    return true;
}

bool pointsToWithinDir(std::filesystem::path const& relativeRoot, std::filesystem::path const& path)
{
    // make absolute and check if it starts with relativeRoot
    const auto absPath = std::filesystem::absolute(path);
    return absPath.string().starts_with(relativeRoot.string());
}

std::optional<std::filesystem::path>
mapUrlToFile(std::filesystem::path const& resourceDir, std::string const& urlPathString)
{
    auto endsWith = [&](std::string_view ending)
    {
        return endsWithImpl(urlPathString, ending);
    };

    const auto path = [&]() -> std::optional<std::filesystem::path>
    {
        // make path relative to / to avoid directory traversal
        if (endsWith(".js") || endsWith(".map") || endsWith(".css") || endsWith(".ttf") || endsWith(".html"))
        {
#ifndef NDEBUG
            return resourceDir / "module_nui-sftp/bin" / std::filesystem::relative(urlPathString, "/");
#endif
            return resourceDir / "frontend" / std::filesystem::relative(urlPathString, "/");
        }
        else
            return resourceDir / "assets" / std::filesystem::relative(urlPathString, "/");

        return std::nullopt;
    }();

    if (path)
    {
        // canonicalize:
        try
        {
            return std::filesystem::canonical(*path);
        }
        catch (std::filesystem::filesystem_error& e)
        {
            Log::error("Failed to canonicalize path '{}': {}", path->string(), e.what());
            return std::nullopt;
        }
    }

    return std::nullopt;
}