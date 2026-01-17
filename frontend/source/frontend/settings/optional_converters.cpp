#include <frontend/settings/optional_converters.hpp>

std::optional<std::string> pathOptionalToStringOptional(std::optional<std::filesystem::path> const& opt)
{
    if (!opt)
        return std::nullopt;
    return opt->generic_string();
}

std::optional<std::filesystem::path> stringOptionalToPathOptional(std::optional<std::string> const& opt)
{
    if (!opt)
        return std::nullopt;
    return std::filesystem::path{*opt};
}

std::optional<unsigned short> filesystemPermsOptionalToUShortOptional(std::optional<std::filesystem::perms> const& opt)
{
    if (!opt)
        return std::nullopt;
    return static_cast<unsigned short>(*opt);
}

std::optional<std::filesystem::perms> uShortOptionalToFilesystemPermsOptional(std::optional<unsigned short> const& opt)
{
    if (!opt)
        return std::nullopt;
    return static_cast<std::filesystem::perms>(*opt);
}

std::optional<long long> durationSecondsOptionalToLongLongOptional(std::optional<std::chrono::seconds> const& opt)
{
    if (!opt)
        return std::nullopt;
    return static_cast<long long>(opt->count());
}

std::optional<std::chrono::seconds> longLongOptionalToDurationSecondsOptional(std::optional<long long> const& opt)
{
    if (!opt)
        return std::nullopt;
    return std::chrono::seconds{*opt};
}