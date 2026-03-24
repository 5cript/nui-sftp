#include <backend/resources.hpp>
#include <roar/filesystem/special_paths.hpp>
#include <log/log.hpp>
#include <constants/persistence.hpp>
#ifndef NDEBUG
#    include <build_environment.hpp>
#endif

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

std::vector<std::filesystem::path> getThemeDirs(std::filesystem::path const& relativeRoot)
{
#ifndef NDEBUG
    return {
        std::filesystem::path{SOURCE_DIR} / "themes",
        Roar::resolvePath(
            std::filesystem::path{"%state_home2%"} / std::filesystem::path{Constants::appName} / "themes"
        ),
    };
#else
    return {
        relativeRoot / "themes",
        Roar::resolvePath(
            std::filesystem::path{"%state_home2%"} / std::filesystem::path{Constants::appName} / "themes"
        ),
    };
#endif
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
        const auto relative = std::filesystem::relative(urlPathString, "/");

        if (endsWith(".css") && relative.parent_path().filename() == "themes")
        {
#ifndef NDEBUG
            std::error_code ec;
            const auto canonical = std::filesystem::canonical(SOURCE_DIR / relative, ec);
            if (ec && !std::filesystem::exists(SOURCE_DIR / relative))
                return Roar::resolvePath("%state_home2%" / std::filesystem::path{Constants::appName} / relative);
            return SOURCE_DIR / relative;
#else
            std::error_code ec;
            const auto canonical = std::filesystem::canonical(resourceDir / relative, ec);
            if (ec && !std::filesystem::exists(resourceDir / relative))
                return Roar::resolvePath("%state_home2%" / std::filesystem::path{Constants::appName} / relative);
            return resourceDir / relative;
#endif
        }

        if (endsWith(".js") || endsWith(".map") || endsWith(".css") || endsWith(".ttf") || endsWith(".html"))
        {
#ifndef NDEBUG
            return resourceDir / "module_nui-sftp/bin" / relative;
#endif
            return resourceDir / "frontend" / relative;
        }
        else
            return resourceDir / "assets" / relative;

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