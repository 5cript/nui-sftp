#pragma once

#include <filesystem>
#include <string>
#include <optional>
#include <vector>

bool isCanonical(std::filesystem::path const& path);

bool pointsToWithinDir(std::filesystem::path const& relativeRoot, std::filesystem::path const& path);

/**
 * @brief Returns the directories that can contain themes. Priority for clashes is in order.
 *
 * @param relativeRoot
 * @return std::vector<std::filesystem::path> Theme dirs.
 */
std::vector<std::filesystem::path> getThemeDirs(std::filesystem::path const& relativeRoot);

std::optional<std::filesystem::path>
mapUrlToFile(std::filesystem::path const& programDir, std::string const& urlPathString);