#pragma once

#include <optional>
#include <filesystem>
#include <chrono>

std::optional<unsigned short> filesystemPermsOptionalToUShortOptional(std::optional<std::filesystem::perms> const& opt);

std::optional<std::filesystem::perms> uShortOptionalToFilesystemPermsOptional(std::optional<unsigned short> const& opt);

std::optional<long long> durationSecondsOptionalToLongLongOptional(std::optional<std::chrono::seconds> const& opt);

std::optional<std::chrono::seconds> longLongOptionalToDurationSecondsOptional(std::optional<long long> const& opt);