#include <utility/extended_length_path.hpp>

#include <string>

namespace Utility
{
#ifdef _WIN32
    std::filesystem::path extendedLengthPath(std::filesystem::path const& path)
    {
        if (isExtendedLengthPath(path))
            return path;
        const std::wstring native = path.lexically_normal().make_preferred().native();
        if (native.starts_with(LR"(\\)"))
            return std::filesystem::path{LR"(\\?\UNC\)" + native.substr(2)};
        if (native.size() >= 3 && native[1] == L':' && native[2] == L'\\')
            return std::filesystem::path{LR"(\\?\)" + native};
        return path;
    }

    bool isExtendedLengthPath(std::filesystem::path const& path)
    {
        auto const& native = path.native();
        return native.starts_with(LR"(\\?\)") || native.starts_with(LR"(\\.\)");
    }

    std::filesystem::path withoutExtendedLengthPrefix(std::filesystem::path const& path)
    {
        auto const& native = path.native();
        if (native.starts_with(LR"(\\?\UNC\)"))
            return std::filesystem::path{LR"(\\)" + native.substr(8)};
        if (native.starts_with(LR"(\\?\)"))
            return std::filesystem::path{native.substr(4)};
        return path;
    }
#else
    std::filesystem::path extendedLengthPath(std::filesystem::path const& path)
    {
        return path;
    }

    bool isExtendedLengthPath(std::filesystem::path const&)
    {
        return false;
    }

    std::filesystem::path withoutExtendedLengthPrefix(std::filesystem::path const& path)
    {
        return path;
    }
#endif
}
