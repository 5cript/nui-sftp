#include <utility/resources.hpp>
#include <roar/filesystem/special_paths.hpp>
#include <constants/persistence.hpp>
#ifndef NDEBUG
#    include <build_environment.hpp>
#endif

#include <ranges>

namespace
{
    auto endsWithImpl(std::string_view pathString, std::string_view ending)
    {
        return pathString.size() >= ending.size() && pathString.substr(pathString.size() - ending.size()) == ending;
    };

    std::vector<std::filesystem::path> getSearchPath(std::filesystem::path const& relativeRoot)
    {
        std::vector<std::filesystem::path> searchPaths;

#ifndef NDEBUG
        searchPaths.push_back(std::filesystem::path{SOURCE_DIR});
        searchPaths.push_back(std::filesystem::path{SOURCE_DIR} / "static");
#endif
        searchPaths.push_back(relativeRoot);
        searchPaths.push_back(
            Roar::resolvePath(std::filesystem::path{"%state_home2%"} / std::filesystem::path{Constants::appName})
        );
        return searchPaths;
    }

    std::vector<std::filesystem::path> transformSearchPaths(
        std::vector<std::filesystem::path> const& searchPaths,
        std::filesystem::path const& relativePath
    )
    {
        return std::ranges::transform_view(
                   searchPaths,
                   [&](std::filesystem::path const& searchPath)
                   {
                       return searchPath / relativePath;
                   }
               ) |
            std::ranges::to<std::vector>();
    }
}

std::vector<std::filesystem::path>
transformedSearchPaths(std::filesystem::path const& relativeRoot, std::filesystem::path const& relativePath)
{
    return transformSearchPaths(getSearchPath(relativeRoot), relativePath);
}

std::optional<std::filesystem::path>
searchInPaths(std::vector<std::filesystem::path> const& searchPaths, std::filesystem::path const& relativePath)
{
    for (const auto& searchPath : searchPaths)
    {
        const auto fullPath = searchPath / relativePath;
        if (std::filesystem::exists(fullPath))
            return fullPath;
    }
    return std::nullopt;
}

std::vector<std::filesystem::path>
searchAllInPaths(std::vector<std::filesystem::path> const& searchPaths, std::filesystem::path const& relativePath)
{
    std::vector<std::filesystem::path> foundPaths;
    for (const auto& searchPath : searchPaths)
    {
        const auto fullPath = searchPath / relativePath;
        if (std::filesystem::exists(fullPath))
            foundPaths.push_back(fullPath);
    }
    return foundPaths;
}

std::vector<std::filesystem::path>
searchAllInPaths(std::filesystem::path const& relativeRoot, std::filesystem::path const& relativePath)
{
    return searchAllInPaths(getSearchPath(relativeRoot), relativePath);
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
    return transformedSearchPaths(relativeRoot, "themes");
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
            return searchInPaths(getThemeDirs(resourceDir), relative.filename());
        }

        if (endsWith(".js") || endsWith(".map") || endsWith(".css") || endsWith(".ttf") || endsWith(".html"))
        {
#ifndef NDEBUG
            return searchInPaths(transformedSearchPaths(resourceDir, "module_nui-sftp/bin"), relative);
#endif
            return searchInPaths(transformedSearchPaths(resourceDir, "frontend"), relative);
        }
        else
            return searchInPaths(transformedSearchPaths(resourceDir, "assets"), relative);

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
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> getAssetsDirectory(std::filesystem::path const& resourceDir)
{
    const auto possibilities = transformedSearchPaths(resourceDir, "assets");
    for (const auto& possibility : possibilities)
    {
        if (std::filesystem::exists(possibility) && std::filesystem::is_directory(possibility))
            return possibility;
    }
    return std::nullopt;
}