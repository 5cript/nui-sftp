#pragma once

#include <filesystem>
#include <string>
#include <optional>
#include <vector>

bool isCanonical(std::filesystem::path const& path);

bool pointsToWithinDir(std::filesystem::path const& relativeRoot, std::filesystem::path const& path);

std::vector<std::filesystem::path>
transformedSearchPaths(std::filesystem::path const& relativeRoot, std::filesystem::path const& relativePath);

std::optional<std::filesystem::path>
searchInPaths(std::vector<std::filesystem::path> const& searchPaths, std::filesystem::path const& relativePath);

std::vector<std::filesystem::path>
searchAllInPaths(std::vector<std::filesystem::path> const& searchPaths, std::filesystem::path const& relativePath);

std::vector<std::filesystem::path>
searchAllInPaths(std::filesystem::path const& relativeRoot, std::filesystem::path const& relativePath);

/**
 * @brief Returns the directories that can contain themes. Priority for clashes is in order.
 *
 * @param relativeRoot
 * @return std::vector<std::filesystem::path> Theme dirs.
 */
std::vector<std::filesystem::path> getThemeDirs(std::filesystem::path const& relativeRoot);

/**
 * @brief Finds all regular files within baseDir matching a relative path pattern with fnmatch-style
 * wildcards (* ?) applied segment by segment. Returns paths relative to baseDir.
 */
std::vector<std::filesystem::path>
matchPatternInDir(std::filesystem::path const& baseDir, std::filesystem::path const& relativePattern);

/**
 * @brief Finds all regular files matching a relative path pattern across multiple search paths.
 * Supports fnmatch-style wildcards (* ?) on individual path segments.
 * Deduplicates by relative path: when the same relative path is found in multiple search paths,
 * only the match from the earliest search path is kept (lower index = higher priority).
 * Returns absolute paths.
 */
std::vector<std::filesystem::path>
findFilesInSearchPaths(
    std::vector<std::filesystem::path> const& searchPaths,
    std::filesystem::path const& relativePattern
);

/**
 * @brief Overload that derives search paths from relativeRoot using the standard search path list.
 */
std::vector<std::filesystem::path>
findFilesInSearchPaths(
    std::filesystem::path const& relativeRoot,
    std::filesystem::path const& relativePattern
);

std::optional<std::filesystem::path>
mapUrlToFile(std::filesystem::path const& programDir, std::string const& urlPathString);

/**
 * @brief Gets all files under the given url (if it points to a directory).
 */
std::vector<std::filesystem::path>
mapDirectoryUrlToFiles(std::filesystem::path const& resourceDir, std::string const& urlPathString);

std::optional<std::filesystem::path> getAssetsDirectory(std::filesystem::path const& resourceDir);