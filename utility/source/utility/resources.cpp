#include <utility/resources.hpp>
#include <nui/backend/filesystem/special_paths.hpp>
#include <constants/persistence.hpp>
#ifndef NDEBUG
#    include <build_environment.hpp>
#endif

#include <ranges>
#include <unordered_set>

namespace
{
    auto endsWithImpl(std::string_view pathString, std::string_view ending)
    {
        return pathString.size() >= ending.size() && pathString.substr(pathString.size() - ending.size()) == ending;
    };

    bool segmentMatchesPattern(std::string_view name, std::string_view pattern)
    {
        if (pattern.empty())
            return name.empty();
        if (pattern[0] == '*')
        {
            for (std::size_t i = 0; i <= name.size(); ++i)
                if (segmentMatchesPattern(name.substr(i), pattern.substr(1)))
                    return true;
            return false;
        }
        if (name.empty())
            return false;
        if (pattern[0] == '?')
            return segmentMatchesPattern(name.substr(1), pattern.substr(1));
        if (pattern[0] != name[0])
            return false;
        return segmentMatchesPattern(name.substr(1), pattern.substr(1));
    }

    bool hasGlobChars(std::string_view s)
    {
        return s.find_first_of("*?") != std::string_view::npos;
    }

    void matchPatternInDirImpl(
        std::filesystem::path const& currentDir,
        std::vector<std::string> const& segments,
        std::size_t index,
        std::filesystem::path const& relative,
        std::vector<std::filesystem::path>& results
    )
    {
        if (index == segments.size())
        {
            std::error_code ec;
            if (std::filesystem::is_regular_file(currentDir, ec))
                results.push_back(relative);
            return;
        }

        const auto& seg = segments[index];
        if (!hasGlobChars(seg))
        {
            const auto next = currentDir / seg;
            std::error_code ec;
            if (std::filesystem::exists(next, ec))
                matchPatternInDirImpl(next, segments, index + 1, relative / seg, results);
        }
        else
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(currentDir, ec))
                return;
            for (const auto& entry : std::filesystem::directory_iterator(currentDir, ec))
            {
                const auto name = entry.path().filename().string();
                if (segmentMatchesPattern(name, seg))
                    matchPatternInDirImpl(entry.path(), segments, index + 1, relative / name, results);
            }
        }
    }

    std::vector<std::filesystem::path> getSearchPath(std::filesystem::path const& relativeRoot)
    {
        std::vector<std::filesystem::path> searchPaths;

#ifndef NDEBUG
        searchPaths.push_back(std::filesystem::path{SOURCE_DIR});
        searchPaths.push_back(std::filesystem::path{SOURCE_DIR} / "static");
#endif
        searchPaths.push_back(relativeRoot);
        searchPaths.push_back(
            Nui::resolvePath(std::filesystem::path{"%state_home2%"} / std::filesystem::path{Constants::appName})
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

std::vector<std::filesystem::path>
matchPatternInDir(std::filesystem::path const& baseDir, std::filesystem::path const& relativePattern)
{
    std::vector<std::string> segments;
    for (auto const& part : relativePattern)
    {
        const auto s = part.string();
        if (!s.empty() && s != "." && s != "/" && s != "\\")
            segments.push_back(s);
    }
    std::vector<std::filesystem::path> results;
    matchPatternInDirImpl(baseDir, segments, 0, {}, results);
    return results;
}

std::vector<std::filesystem::path> findFilesInSearchPaths(
    std::vector<std::filesystem::path> const& searchPaths,
    std::filesystem::path const& relativePattern
)
{
    std::unordered_set<std::string> seen;
    std::vector<std::filesystem::path> result;
    for (const auto& searchPath : searchPaths)
    {
        for (auto const& relPath : matchPatternInDir(searchPath, relativePattern))
        {
            if (seen.insert(relPath.generic_string()).second)
                result.push_back(searchPath / relPath);
        }
    }
    return result;
}

std::vector<std::filesystem::path>
findFilesInSearchPaths(std::filesystem::path const& relativeRoot, std::filesystem::path const& relativePattern)
{
    return findFilesInSearchPaths(getSearchPath(relativeRoot), relativePattern);
}

std::optional<std::filesystem::path>
mapUrlToFile(std::filesystem::path const& resourceDir, std::string const& urlPathString)
{
    auto endsWith = [&](std::string_view ending)
    {
        return endsWithImpl(urlPathString, ending);
    };
    auto firstOf = [](std::vector<std::filesystem::path> const& v) -> std::optional<std::filesystem::path>
    {
        return v.empty() ? std::nullopt : std::optional{v[0]};
    };

    const auto path = [&]() -> std::optional<std::filesystem::path>
    {
        // make path relative to / to avoid directory traversal
        const auto relative = std::filesystem::relative(urlPathString, "/");

        if (endsWith(".css") && relative.parent_path().filename() == "themes")
            return firstOf(findFilesInSearchPaths(getThemeDirs(resourceDir), relative.filename()));

        if (endsWith(".js") || endsWith(".map") || endsWith(".css") || endsWith(".ttf") || endsWith(".html"))
        {
#ifndef NDEBUG
            return firstOf(
                findFilesInSearchPaths(transformedSearchPaths(resourceDir, "module_nui-sftp/bin"), relative)
            );
#endif
            return firstOf(findFilesInSearchPaths(transformedSearchPaths(resourceDir, "frontend"), relative));
        }
        else
            return firstOf(findFilesInSearchPaths(transformedSearchPaths(resourceDir, "assets"), relative));

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