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

std::optional<std::filesystem::path>
mapUrlToFile(std::filesystem::path const& programDir, std::string const& urlPathString);

/**
 * @brief Gets all files under the given url (if it points to a directory).
 */
std::vector<std::filesystem::path>
mapDirectoryUrlToFiles(std::filesystem::path const& resourceDir, std::string const& urlPathString);

std::optional<std::filesystem::path> getAssetsDirectory(std::filesystem::path const& resourceDir);