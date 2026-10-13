#pragma once

#include <utility/describe.hpp>
#include <shared_data/directory_entry.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <cstdint>
#include <chrono>

namespace NuiFileExplorer
{
    struct Item : public SharedData::DirectoryEntry
    {
        using Type = SharedData::DirectoryEntry::FileType;
        std::string icon; // url or base64 url etc.
        /** @brief The absolute path of the item on its side, set by the side model that lists it. */
        std::filesystem::path fullPath{};
        /** @brief The entry leading to the parent directory, shown as "..". It is never selected or acted on. */
        bool isParentLink{false};

        explicit Item(SharedData::DirectoryEntry const& entry, std::string icon = {})
            : SharedData::DirectoryEntry{entry}
            , icon{std::move(icon)}
        {}

        /**
         * @brief The entry leading to @p parentDirectory.
         */
        static Item parentLink(std::filesystem::path parentDirectory, std::string icon = {})
        {
            Item item{SharedData::DirectoryEntry{.type = Type::Directory}, std::move(icon)};
            item.fullPath = std::move(parentDirectory);
            item.isParentLink = true;
            return item;
        }

        /**
         * @brief An item dropped from outside, known by its absolute path only. Separators of both platforms
         *        count, the frontend cannot know which one the path came from.
         */
        static Item dropped(SharedData::DirectoryEntry entry, std::string const& absolutePath)
        {
            std::string_view trimmed{absolutePath};
            while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\'))
                trimmed.remove_suffix(1);
            const auto separator = trimmed.find_last_of("/\\");
            entry.name = std::string{separator == std::string_view::npos ? trimmed : trimmed.substr(separator + 1)};
            Item item{entry};
            item.fullPath = std::filesystem::path{std::string{trimmed}};
            return item;
        }

        std::string displayName() const
        {
            return isParentLink ? std::string{".."} : name;
        }
    };
}
