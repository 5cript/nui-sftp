#include <backend/process/boost_process.hpp>

#include <backend/process/environment.hpp>
#include <paths/utf8.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bp2 = boost::process::v2;

namespace
{
    constexpr std::string_view originalValuePrefix = "NUI_SFTP_APPIMAGE_ORIGINAL_";
    constexpr std::string_view unsetMarkerPrefix = "NUI_SFTP_APPIMAGE_UNSET_";

    /**
     * @brief The text of an environment key or value as UTF-8, converted from the wide text on Windows.
     */
    template <typename ViewT>
    std::string utf8Of(ViewT const& view)
    {
        const auto native = view.native();
#ifdef _WIN32
        return Paths::wideToUtf8(std::wstring_view{native.data(), native.size()});
#else
        return std::string{native.data(), native.size()};
#endif
    }
}

Environment::Environment(
    bool clean,
    std::unordered_map<std::string, std::string> const& mergeIn,
    std::string const& pathMergeIn)
    : environment_{}
{
    if (!clean)
        loadFromCurrent();

    merge(mergeIn, true);
    extendPath(pathMergeIn);
}
std::unordered_map<std::string, std::string>& Environment::environment()
{
    return environment_;
}
void Environment::loadFromCurrent()
{
    environment_ = {};
    const auto currentEnv = bp2::environment::current();
    for (auto iter = currentEnv.begin(); iter != currentEnv.end(); ++iter)
    {
        auto deref = *iter;
        environment_.emplace(utf8Of(deref.key()), utf8Of(deref.value()));
    }
    restoreHostEnvironment();
}
void Environment::restoreHostEnvironment()
{
    std::vector<std::pair<std::string, std::optional<std::string>>> restorations;
    for (auto iter = environment_.begin(); iter != environment_.end();)
    {
        auto const& [key, value] = *iter;
        if (key.starts_with(originalValuePrefix))
            restorations.emplace_back(key.substr(originalValuePrefix.size()), value);
        else if (key.starts_with(unsetMarkerPrefix))
            restorations.emplace_back(key.substr(unsetMarkerPrefix.size()), std::nullopt);
        else
        {
            ++iter;
            continue;
        }
        iter = environment_.erase(iter);
    }

    for (auto const& [name, value] : restorations)
    {
        if (value)
            environment_[name] = *value;
        else
            environment_.erase(name);
    }
}
void Environment::extendPath(std::string const& path, bool front)
{
    auto iter = environment_.find("PATH");
    if (iter == environment_.end())
    {
        environment_["PATH"] = path;
    }
    else
    {
        if (front)
            iter->second = path + ":" + iter->second;
        else
            iter->second += ":" + path;
    }
}
void Environment::merge(std::unordered_map<std::string, std::string> const& other, bool overwrite)
{
    for (auto const& [key, value] : other)
    {
        if (overwrite || environment_.find(key) == environment_.end())
        {
            environment_[key] = value;
        }
    }
}