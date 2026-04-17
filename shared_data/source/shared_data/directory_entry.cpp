#include <shared_data/directory_entry.hpp>

#include <utility/path_utf.hpp>

namespace SharedData
{
    // Paths cross the RPC boundary as UTF-8. In-memory they are std::filesystem::path (native
    // encoding: UTF-16 on Windows, UTF-8 on POSIX), so conversion happens here and nowhere else.
    // Default nlohmann serialization of fs::path on MSVC uses path.string(), which goes through
    // the active code page — that would corrupt any non-ASCII character, hence the explicit
    // Utility::pathToUtf8Generic / pathFromUtf8 calls.

    void to_json(nlohmann::json& j, DirectoryEntry const& entry)
    {
        j = nlohmann::json{
            {"path", Utility::pathToUtf8Generic(entry.path)},
            {"type", entry.type},
            {"size", entry.size},
            {"permissions", entry.permissions},
            {"mtime", entry.mtime},
            {"mtimeNsec", entry.mtimeNsec},
        };
        if (!entry.fullPath.empty())
            j["fullPath"] = Utility::pathToUtf8Generic(entry.fullPath);
        if (!entry.longName.empty())
            j["longName"] = Utility::pathToUtf8Generic(entry.longName);
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
            j["linkTarget"] = Utility::pathToUtf8Generic(*entry.linkTarget);
        if (entry.resolvedTarget)
            j["resolvedTarget"] = *entry.resolvedTarget;
    }
    void from_json(nlohmann::json const& j, DirectoryEntry& entry)
    {
        const auto getOptionalPath = [&](char const* key, std::filesystem::path& target) {
            if (auto found = j.find(key); found != j.end() && !found->is_null())
                target = Utility::pathFromUtf8(found->get<std::string>());
        };
        const auto getOptional = [&](char const* key, auto& target) {
            if (auto found = j.find(key); found != j.end() && !found->is_null())
                found->get_to(target);
        };

        entry.path = Utility::pathFromUtf8(j.at("path").get<std::string>());
        j.at("type").get_to(entry.type);
        getOptionalPath("fullPath", entry.fullPath);
        getOptionalPath("longName", entry.longName);
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
            entry.linkTarget = Utility::pathFromUtf8(found->get<std::string>());
        if (auto found = j.find("resolvedTarget"); found != j.end() && !found->is_null())
        {
            DirectoryEntry target{};
            found->get_to(target);
            entry.resolvedTarget = std::make_shared<DirectoryEntry>(std::move(target));
        }
    }
}