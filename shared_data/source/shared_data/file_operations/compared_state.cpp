#include <shared_data/file_operations/compared_state.hpp>

namespace SharedData
{
    ComparedState comparedStateOf(DirectoryEntry const& entry)
    {
        return ComparedState{
            .exists = true,
            .type = entry.type,
            .size = entry.size,
            .mtime = entry.mtime,
            .linkTarget = entry.type == FileType::Symlink ? entry.linkTarget : std::nullopt,
        };
    }

    bool unchangedSinceCompare(ComparedState const& compared, ComparedState const& current)
    {
        if (!compared.exists)
            return !current.exists;
        if (!current.exists)
            return true;
        if (compared.type != current.type)
            return false;
        switch (compared.type)
        {
            case FileType::Directory:
                return true;
            case FileType::Symlink:
                return compared.linkTarget == current.linkTarget;
            default:
                return compared.size == current.size && compared.mtime == current.mtime;
        }
    }
}
