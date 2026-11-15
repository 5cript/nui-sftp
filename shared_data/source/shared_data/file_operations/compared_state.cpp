#include <shared_data/file_operations/compared_state.hpp>

#include <algorithm>

namespace SharedData
{
    ComparedState comparedStateOf(DirectoryEntry const& entry)
    {
        return ComparedState{
            .exists = true,
            .type = entry.type,
            .size = entry.size,
            .mtime = entry.mtime,
            .mtimePrecision = entry.mtimePrecision,
            .linkTarget = entry.type == FileType::Symlink ? entry.linkTarget : std::nullopt,
        };
    }

    namespace
    {
        bool sameTime(ComparedState const& compared, ComparedState const& current)
        {
            const auto unit = std::max(secondsOf(compared.mtimePrecision), secondsOf(current.mtimePrecision));
            return compared.mtime / unit == current.mtime / unit;
        }
    }

    bool unchangedSinceCompare(ComparedState const& compared, ComparedState const& current)
    {
        if (!compared.exists)
            return !current.exists;
        if (!current.exists)
            return !compared.mustStillExist;
        if (compared.type != current.type)
            return false;
        switch (compared.type)
        {
            case FileType::Directory:
                return true;
            case FileType::Symlink:
                return compared.linkTarget == current.linkTarget;
            default:
                return compared.size == current.size && sameTime(compared, current);
        }
    }
}
