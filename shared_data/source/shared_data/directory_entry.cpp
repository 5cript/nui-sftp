#include <shared_data/directory_entry.hpp>

namespace SharedData
{
    void to_json(nlohmann::json& j, DirectoryEntry const& entry)
    {
        j = nlohmann::json{
            {"path", entry.path},
            {"type", entry.type},
            {"size", entry.size},
            {"permissions", entry.permissions},
            {"mtime", entry.mtime},
            {"mtimeNsec", entry.mtimeNsec},
        };
        if (!entry.fullPath.empty())
            j["fullPath"] = entry.fullPath;
        if (!entry.longName.empty())
            j["longName"] = entry.longName;
        if (entry.flags != 0)
            j["flags"] = entry.flags;
        if (entry.uid != 0)
            j["uid"] = entry.uid;
        if (entry.gid != 0)
            j["gid"] = entry.gid;
        if (!entry.owner.empty())
            j["owner"] = entry.owner;
        if (!entry.group.empty())
            j["group"] = entry.group;
        if (entry.atime != 0)
            j["atime"] = entry.atime;
        if (entry.atimeNsec != 0)
            j["atimeNsec"] = entry.atimeNsec;
        if (entry.createTime != 0)
            j["createTime"] = entry.createTime;
        if (entry.createTimeNsec != 0)
            j["createTimeNsec"] = entry.createTimeNsec;
        if (!entry.acl.empty())
            j["acl"] = entry.acl;
        if (entry.linkTarget)
            j["linkTarget"] = entry.linkTarget->generic_string();
        if (entry.resolvedTarget)
            j["resolvedTarget"] = *entry.resolvedTarget;
    }
    void from_json(nlohmann::json const& j, DirectoryEntry& entry)
    {
        const auto getOptional = [&](char const* key, auto& target) {
            if (auto found = j.find(key); found != j.end() && !found->is_null())
                found->get_to(target);
        };

        j.at("path").get_to(entry.path);
        j.at("type").get_to(entry.type);
        getOptional("fullPath", entry.fullPath);
        getOptional("longName", entry.longName);
        getOptional("flags", entry.flags);
        getOptional("size", entry.size);
        getOptional("uid", entry.uid);
        getOptional("gid", entry.gid);
        getOptional("owner", entry.owner);
        getOptional("group", entry.group);
        getOptional("permissions", entry.permissions);
        getOptional("atime", entry.atime);
        getOptional("atimeNsec", entry.atimeNsec);
        getOptional("createTime", entry.createTime);
        getOptional("createTimeNsec", entry.createTimeNsec);
        getOptional("mtime", entry.mtime);
        getOptional("mtimeNsec", entry.mtimeNsec);
        getOptional("acl", entry.acl);
        if (auto found = j.find("linkTarget"); found != j.end() && !found->is_null())
        {
            std::string tgt;
            found->get_to(tgt);
            entry.linkTarget = std::filesystem::path{tgt};
        }
        if (auto found = j.find("resolvedTarget"); found != j.end() && !found->is_null())
        {
            DirectoryEntry target{};
            found->get_to(target);
            entry.resolvedTarget = std::make_shared<DirectoryEntry>(std::move(target));
        }
    }
}