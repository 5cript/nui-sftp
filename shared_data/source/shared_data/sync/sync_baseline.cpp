#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/sync_baseline.hpp>

#include <string>
#include <utility>

namespace SharedData::Sync
{
    namespace
    {
        std::string joinRelKey(std::string const& parent, std::string const& name)
        {
            return parent.empty() ? name : fmt::format("{}/{}", parent, name);
        }

        void collectAgreement(
            std::vector<ScanNode> const& localChildren,
            std::vector<ScanNode> const& remoteChildren,
            std::string const& parentRelKey,
            SyncBaseline& baseline
        )
        {
            auto localIter = localChildren.begin();
            auto remoteIter = remoteChildren.begin();
            while (localIter != localChildren.end() && remoteIter != remoteChildren.end())
            {
                if (localIter->name < remoteIter->name)
                {
                    ++localIter;
                    continue;
                }
                if (remoteIter->name < localIter->name)
                {
                    ++remoteIter;
                    continue;
                }
                auto const& local = *localIter++;
                auto const& remote = *remoteIter++;
                const bool listed =
                    local.availability == Availability::Listed && remote.availability == Availability::Listed;
                const bool directories = local.type == FileType::Directory && remote.type == FileType::Directory;
                if (!listed || local.type != remote.type || (!directories && entriesDiffer(local, remote)))
                    continue;
                auto relKey = joinRelKey(parentRelKey, local.name);
                if (directories)
                    collectAgreement(local.children, remote.children, relKey, baseline);
                baseline.records.insert_or_assign(
                    std::move(relKey), SyncRecord{.local = recordedSideOf(local), .remote = recordedSideOf(remote)}
                );
            }
        }

        nlohmann::json sideToJson(RecordedSide const& side)
        {
            return nlohmann::json::array({static_cast<int>(side.type), side.size, side.mtime});
        }

        RecordedSide sideFromJson(nlohmann::json const& json, std::size_t offset)
        {
            return RecordedSide{
                .type = static_cast<FileType>(json.at(offset).get<int>()),
                .size = json.at(offset + 1).get<std::uint64_t>(),
                .mtime = json.at(offset + 2).get<std::uint64_t>(),
            };
        }

        std::optional<std::string> linkTargetFromJson(nlohmann::json const& json, std::size_t index)
        {
            if (json.size() <= index || json.at(index).is_null())
                return std::nullopt;
            return json.at(index).get<std::string>();
        }
    }

    RecordedSide recordedSideOf(ScanNode const& node)
    {
        return RecordedSide{
            .type = node.type,
            .size = node.type == FileType::Directory ? 0u : node.size,
            .mtime = node.type == FileType::Directory ? 0u : node.mtime,
            .linkTarget = node.type == FileType::Symlink ? node.linkTarget : std::nullopt,
        };
    }

    bool sideChanged(RecordedSide const& recorded, ScanNode const& current)
    {
        if (recorded.type != current.type)
            return true;
        switch (current.type)
        {
            case FileType::Directory:
                return false;
            case FileType::Symlink:
                return recorded.linkTarget != current.linkTarget;
            default:
                return recorded.size != current.size || recorded.mtime != current.mtime;
        }
    }

    SyncRecord transferredRecordOf(ScanNode const& source)
    {
        const auto side = recordedSideOf(source);
        return SyncRecord{.local = side, .remote = side};
    }

    SyncBaseline agreementOf(ScanNode const& local, ScanNode const& remote)
    {
        SyncBaseline baseline;
        collectAgreement(local.children, remote.children, std::string{}, baseline);
        return baseline;
    }

    void to_json(nlohmann::json& json, SyncBaseline const& baseline)
    {
        auto records = nlohmann::json::object();
        for (auto const& [relKey, record] : baseline.records)
        {
            auto entry = sideToJson(record.local);
            const auto remote = sideToJson(record.remote);
            entry.insert(entry.end(), remote.begin(), remote.end());
            if (record.local.linkTarget || record.remote.linkTarget)
            {
                entry.push_back(record.local.linkTarget ? nlohmann::json(*record.local.linkTarget) : nlohmann::json());
                entry.push_back(
                    record.remote.linkTarget ? nlohmann::json(*record.remote.linkTarget) : nlohmann::json()
                );
            }
            records[relKey] = std::move(entry);
        }
        json = nlohmann::json{{"records", std::move(records)}};
    }

    void from_json(nlohmann::json const& json, SyncBaseline& baseline)
    {
        baseline.records.clear();
        for (auto const& [relKey, entry] : json.at("records").items())
        {
            auto local = sideFromJson(entry, 0);
            auto remote = sideFromJson(entry, 3);
            local.linkTarget = linkTargetFromJson(entry, 6);
            remote.linkTarget = linkTargetFromJson(entry, 7);
            baseline.records.insert_or_assign(
                relKey, SyncRecord{.local = std::move(local), .remote = std::move(remote)}
            );
        }
    }
}
