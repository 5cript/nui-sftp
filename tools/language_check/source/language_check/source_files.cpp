#include <language_check/source_files.hpp>

#include <build_environment.hpp>

#include <fnmatch.h>
#include <fstream>

namespace
{
    std::vector<std::string> readGitIgnore()
    {
        std::vector<std::string> gitIgnoreEntries;
        std::filesystem::path gitIgnorePath = std::filesystem::path(SOURCE_DIR) / ".gitignore";

        if (std::filesystem::exists(gitIgnorePath))
        {
            std::ifstream gitIgnoreFile(gitIgnorePath, std::ios_base::binary);
            std::string line;
            while (std::getline(gitIgnoreFile, line))
            {
                if (!line.empty() && line[0] != '#') // Ignore comments and empty lines
                {
                    // strip all whitespace:
                    line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
                    gitIgnoreEntries.push_back(line);
                }
            }
        }

        gitIgnoreEntries.push_back(".git");

        return gitIgnoreEntries;
    }

    bool isIgnored(
        const std::filesystem::path& path,
        const std::filesystem::path& root,
        const std::vector<std::string>& gitIgnores
    )
    {
        const auto relativePath = std::filesystem::relative(path, root).generic_string();
        for (const auto& pattern : gitIgnores)
        {
            if (fnmatch(pattern.c_str(), relativePath.c_str(), 0) == 0)
                return true;
        }
        return false;
    }
}

std::vector<std::filesystem::path> deepScanSources(const std::filesystem::path& directory)
{
    const auto gitIgnores = readGitIgnore();

    std::vector<std::filesystem::path> sources;
    auto it = std::filesystem::recursive_directory_iterator(directory);
    for (auto& entry : it)
    {
        if (entry.is_directory())
        {
            if (isIgnored(entry.path(), directory, gitIgnores))
                it.disable_recursion_pending();
            continue;
        }

        if (entry.is_regular_file() && !isIgnored(entry.path(), directory, gitIgnores))
        {
            if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")
                sources.push_back(entry.path());
        }
    }
    return sources;
}