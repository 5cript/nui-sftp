#include <shared_data/directory_entry.hpp>

namespace SharedData
{
    void to_json(nlohmann::json& j, DirectoryEntry const& entry)
    {
        j = nlohmann::json{
            {"path", entry.path},
            {"fullPath", entry.fullPath},
            {"longName", entry.longName},
            {"flags", entry.flags},
            {"type", entry.type},
            {"size", entry.size},
            {"uid", entry.uid},
            {"gid", entry.gid},
            {"owner", entry.owner},
            {"group", entry.group},
            {"permissions", entry.permissions},
            {"atime", entry.atime},
            {"atimeNsec", entry.atimeNsec},
            {"createTime", entry.createTime},
            {"createTimeNsec", entry.createTimeNsec},
            {"mtime", entry.mtime},
            {"mtimeNsec", entry.mtimeNsec},
            {"acl", entry.acl},
            {"linkTarget", entry.linkTarget ? nlohmann::json(entry.linkTarget->generic_string()) : nlohmann::json(nullptr)},
            {"resolvedTarget", entry.resolvedTarget ? nlohmann::json(*entry.resolvedTarget) : nlohmann::json(nullptr)},
        };
    }
    void from_json(nlohmann::json const& j, DirectoryEntry& entry)
    {
        j.at("path").get_to(entry.path);
        if (auto it = j.find("fullPath"); it != j.end())
            it->get_to(entry.fullPath);
        j.at("longName").get_to(entry.longName);
        j.at("flags").get_to(entry.flags);
        j.at("type").get_to(entry.type);
        j.at("size").get_to(entry.size);
        j.at("uid").get_to(entry.uid);
        j.at("gid").get_to(entry.gid);
        j.at("owner").get_to(entry.owner);
        j.at("group").get_to(entry.group);
        j.at("permissions").get_to(entry.permissions);
        j.at("atime").get_to(entry.atime);
        j.at("atimeNsec").get_to(entry.atimeNsec);
        j.at("createTime").get_to(entry.createTime);
        j.at("createTimeNsec").get_to(entry.createTimeNsec);
        j.at("mtime").get_to(entry.mtime);
        j.at("mtimeNsec").get_to(entry.mtimeNsec);
        j.at("acl").get_to(entry.acl);
        if (auto it = j.find("linkTarget"); it != j.end() && !it->is_null())
        {
            std::string tgt;
            it->get_to(tgt);
            entry.linkTarget = std::filesystem::path{tgt};
        }
        if (auto it = j.find("resolvedTarget"); it != j.end() && !it->is_null())
        {
            DirectoryEntry target{};
            it->get_to(target);
            entry.resolvedTarget = std::make_shared<DirectoryEntry>(std::move(target));
        }
    }
}