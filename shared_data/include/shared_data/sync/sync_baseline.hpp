#pragma once

#include <shared_data/sync/scan_node.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace SharedData::Sync
{
    /**
     * @brief One side of an entry as sync left it, compared with the next scan of the same side only, so the
     *        clocks of the two sides never meet.
     */
    struct RecordedSide
    {
        FileType type{FileType::Unknown};
        std::uint64_t size{0};
        std::uint64_t mtime{0};
        /** @brief The link literal of a symlink, which compares by it instead of size and time. */
        std::optional<std::string> linkTarget{};

        bool operator==(RecordedSide const&) const = default;
    };

    /**
     * @brief Both sides of an entry after it was last synchronized or found in agreement.
     */
    struct SyncRecord
    {
        RecordedSide local{};
        RecordedSide remote{};

        bool operator==(SyncRecord const&) const = default;
    };

    /**
     * @brief What sync knows about one pair (saved session, local root, remote root) from earlier runs: a record per
     *        relKey for every entry that was synchronized successfully or found equal on both sides. An entry without
     *        a record was never in agreement.
     */
    struct SyncBaseline
    {
        std::unordered_map<std::string, SyncRecord> records{};

        bool operator==(SyncBaseline const&) const = default;
    };

    /**
     * @brief How @p node is recorded: type, size and modification time, the link literal for a symlink.
     */
    RecordedSide recordedSideOf(ScanNode const& node);

    /**
     * @brief True when @p current is not what @p recorded holds. A directory changes only by becoming something
     *        else, its modification time and size do not count. A symlink compares by its literal.
     */
    bool sideChanged(RecordedSide const& recorded, ScanNode const& current);

    /**
     * @brief The record of an entry transferred from @p source: both sides hold what the source held.
     */
    SyncRecord transferredRecordOf(ScanNode const& source);

    /**
     * @brief Records every entry listed on both sides that the diff sees as equal, directories included, walking
     *        into directories present on both sides.
     */
    SyncBaseline agreementOf(ScanNode const& local, ScanNode const& remote);

    void to_json(nlohmann::json& json, SyncBaseline const& baseline);
    void from_json(nlohmann::json const& json, SyncBaseline& baseline);
}
