#pragma once

#include <filesystem>
#include <vector>

std::vector<std::filesystem::path> deepScanSources(const std::filesystem::path& directory);
