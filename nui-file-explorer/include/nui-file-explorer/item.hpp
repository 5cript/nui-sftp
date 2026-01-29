#pragma once

#include <utility/describe.hpp>
#include <shared_data/directory_entry.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <filesystem>
#include <string>
#include <cstdint>
#include <chrono>

namespace NuiFileExplorer
{
    struct Item : public SharedData::DirectoryEntry
    {
        using Type = SharedData::DirectoryEntry::FileType;
        std::string icon; // url or base64 url etc.

        Item(SharedData::DirectoryEntry const& entry, std::string icon = {})
            : SharedData::DirectoryEntry{entry}
            , icon{std::move(icon)}
        {}
    };
}