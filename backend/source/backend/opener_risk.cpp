#include <backend/opener_risk.hpp>
#include <paths/native.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

namespace OpenerDetail
{
    bool hasExtensionIn(std::filesystem::path const& path, std::span<std::string_view const> extensions)
    {
        const auto extension = [&path]()
        {
            auto text = Paths::nativeToUtf8(path.extension());
            std::ranges::transform(
                text,
                text.begin(),
                [](char character)
                {
                    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
                }
            );
            return text;
        }();
        if (extension.empty())
            return false;
        return std::ranges::find(extensions, std::string_view{extension}) != extensions.end();
    }

    bool hasExecuteBit(std::filesystem::perms permissions)
    {
        using std::filesystem::perms;
        return (permissions & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
    }

    bool hasExecutableContent(std::filesystem::path const& path)
    {
        std::ifstream reader{path, std::ios::binary};
        std::array<char, 4> start{};
        reader.read(start.data(), start.size());
        const auto length = reader.gcount();
        const bool isElf = length == 4 && start[0] == '\x7f' && start[1] == 'E' && start[2] == 'L' && start[3] == 'F';
        const bool isScript = length >= 2 && start[0] == '#' && start[1] == '!';
        return isElf || isScript;
    }

    std::optional<std::string> linuxRiskOf(std::filesystem::path const& path)
    {
        std::error_code statusError{};
        const auto status = std::filesystem::status(path, statusError);
        if (statusError || !std::filesystem::is_regular_file(status))
            return std::nullopt;
        if (hasExecuteBit(status.permissions()) && hasExecutableContent(path))
            return std::string{"backend.opener.risk.program"};
        std::error_code resolveError{};
        const auto target = std::filesystem::canonical(path, resolveError);
        if (hasExtensionIn(path, linuxRunnableExtensions) ||
            (!resolveError && hasExtensionIn(target, linuxRunnableExtensions)))
            return std::string{"backend.opener.risk.dangerousType"};
        return std::nullopt;
    }
}
