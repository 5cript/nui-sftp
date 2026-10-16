#pragma once

#include <nui-file-explorer/error.hpp>
#include <nui-file-explorer/item.hpp>

#include <expected>
#include <optional>
#include <string>

namespace NuiFileExplorer
{
    /**
     * @brief Where items dropped or pasted onto @p item go, relative to the directory its side shows: ".." for the
     *        parent entry, the name of a directory or of a link to one, nothing for other items, which leaves them in
     *        the directory itself. A directory whose name is not valid UTF-8 cannot be named, so it is no target.
     */
    inline std::expected<std::optional<std::string>, Error> transferTargetOf(Item const& item)
    {
        if (item.isParentLink)
            return std::string{".."};
        if (!item.isDirectoryLike())
            return std::nullopt;
        if (!item.nameIsValidUtf8)
            return std::unexpected(Error{.code = ErrorCode::DropTargetNameInvalid, .detail = item.name});
        return item.name;
    }
}
