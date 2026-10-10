#include <backend/file_tracking/change_filter.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief AppKit's safe save (TextEdit and others) writes "<name>.sb-<8 hex digits>-<6 characters>" first.
         */
        bool isSafeSaveTemporary(std::string_view name)
        {
            const auto marker = name.rfind(".sb-");
            if (marker == std::string_view::npos)
                return false;
            const auto suffix = name.substr(marker + 4);
            if (suffix.size() != 15 || suffix[8] != '-')
                return false;
            const auto isHex = [](char character)
            {
                return std::isxdigit(static_cast<unsigned char>(character)) != 0;
            };
            const auto isAlphanumeric = [](char character)
            {
                return std::isalnum(static_cast<unsigned char>(character)) != 0;
            };
            return std::all_of(suffix.begin(), suffix.begin() + 8, isHex) &&
                std::all_of(suffix.begin() + 9, suffix.end(), isAlphanumeric);
        }
    }

    bool isTransientFile(std::string const& filename, std::string const& downloadTempSuffix)
    {
        const auto name = std::filesystem::path{filename}.filename().string();
        if (name.starts_with(".goutputstream"))
            return true;
        if (!downloadTempSuffix.empty() && name.size() > downloadTempSuffix.size() &&
            name.ends_with(downloadTempSuffix))
            return true;
        if (name == ".DS_Store" || name.starts_with("._"))
            return true;
        return isSafeSaveTemporary(name);
    }

    std::optional<FileChange>
    filterFileChange(FileChange change, bool isDirectory, std::string const& downloadTempSuffix)
    {
        if (isTransientFile(change.filename, downloadTempSuffix))
            return std::nullopt;

        if (change.action == FileAction::Moved && isTransientFile(change.oldFilename, downloadTempSuffix))
        {
            // A finished download renames its temporary: remote content arriving, nothing to mirror. FSEvents even
            // reports the ones from just before the watch started.
            if (change.oldFilename == change.filename + downloadTempSuffix)
                return std::nullopt;
            // An atomic save renames its temporary over the real file, which is a change of that file.
            change.action = FileAction::Modified;
            change.oldFilename.clear();
        }

        // Windows and macOS report a directory as modified when entries inside it change, the entries report
        // themselves.
        if (change.action == FileAction::Modified && isDirectory)
            return std::nullopt;

        return change;
    }
}
