#pragma once

#include <filesystem>
#include <string>
#include <optional>

bool isCanonical(std::filesystem::path const& path);

bool pointsToWithinDir(std::filesystem::path const& relativeRoot, std::filesystem::path const& path);

std::optional<std::filesystem::path>
mapUrlToFile(std::filesystem::path const& programDir, std::string const& urlPathString);