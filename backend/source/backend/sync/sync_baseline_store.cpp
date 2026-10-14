#include <backend/sync/sync_baseline_store.hpp>
#include <constants/persistence.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>
#include <utility/rename.hpp>

#include <nui/backend/filesystem/special_paths.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr int formatVersion = 1;

    std::mutex& storeMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    /**
     * @brief The session part of a pair key, see SyncBaselineStore::pairKey.
     */
    std::string_view sessionOf(std::string_view pairKey)
    {
        return pairKey.substr(0, pairKey.find('\n'));
    }

    std::uint64_t fnv1a(std::string_view text)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto character : text)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    /**
     * @brief The file's JSON, nothing when it does not exist, an error text when it cannot be read.
     */
    std::expected<std::optional<nlohmann::json>, std::string> readJson(std::filesystem::path const& file)
    {
        std::error_code error{};
        if (!std::filesystem::exists(file, error))
        {
            if (error)
                return std::unexpected(Utility::errorMessage(error));
            return std::nullopt;
        }
        std::ifstream stream{file, std::ios::binary};
        if (!stream)
            return std::unexpected(std::string{"cannot open"});
        auto json = nlohmann::json::parse(stream, nullptr, false);
        if (json.is_discarded())
            return std::unexpected(std::string{"not JSON"});
        return json;
    }
}

SyncBaselineStore::SyncBaselineStore(std::filesystem::path directory)
    : directory_{std::move(directory)}
{}

std::filesystem::path SyncBaselineStore::defaultDirectory()
{
    return Nui::resolvePath(std::filesystem::path{std::string{Constants::syncBaselinePath}});
}

std::string SyncBaselineStore::pairKey(
    std::string_view sessionName,
    Paths::LocalPath const& localRoot,
    Paths::RemotePath const& remoteRoot
)
{
    return fmt::format("{}\n{}\n{}", sessionName, localRoot.toUtf8(), remoteRoot.value());
}

std::filesystem::path SyncBaselineStore::fileOf(std::filesystem::path const& directory, std::string_view pairKey)
{
    return directory / fmt::format("{:016x}-{:016x}.json", fnv1a(sessionOf(pairKey)), fnv1a(pairKey));
}

std::size_t SyncBaselineStore::pruneAllBut(std::vector<std::string> const& sessionNames) const
{
    std::scoped_lock lock{storeMutex()};
    std::unordered_set<std::string> kept;
    for (auto const& name : sessionNames)
        kept.insert(fmt::format("{:016x}-", fnv1a(name)));

    std::vector<std::filesystem::path> orphans;
    std::error_code error{};
    for (
        auto iter = std::filesystem::directory_iterator{directory_, error};
        !error && iter != std::filesystem::directory_iterator{};
        iter.increment(error)
    )
    {
        const auto name = Paths::nativeToUtf8(iter->path().filename());
        // Session hash, a dash, pair hash and ".json".
        if (name.size() == 16 + 1 + 16 + 5 && name.ends_with(".json") && !kept.contains(name.substr(0, 17)))
            orphans.push_back(iter->path());
    }

    std::size_t removed = 0;
    for (auto const& orphan : orphans)
    {
        std::error_code removeError{};
        if (std::filesystem::remove(orphan, removeError))
            ++removed;
        else if (removeError)
            Log::warn(
                "Cannot remove the sync baseline '{}': {}",
                Paths::nativeToUtf8(orphan),
                Utility::errorMessage(removeError)
            );
    }
    if (removed > 0)
        Log::info("Removed {} sync baselines of sessions that no longer exist.", removed);
    return removed;
}

std::optional<SharedData::Sync::SyncBaseline> SyncBaselineStore::loadLocked(std::string const& pairKey) const
{
    const auto file = fileOf(directory_, pairKey);
    const auto json = readJson(file);
    if (!json)
    {
        Log::error(
            "Sync baseline '{}' cannot be read ({}), every delete of its pair is refused.",
            Paths::nativeToUtf8(file),
            json.error()
        );
        return SharedData::Sync::SyncBaseline{};
    }
    if (!*json)
        return std::nullopt;
    auto const& content = **json;
    if (!content.is_object() || content.value("pair", std::string{}) != pairKey)
    {
        Log::warn("Sync baseline '{}' belongs to another pair, ignored.", Paths::nativeToUtf8(file));
        return std::nullopt;
    }
    try
    {
        return content.get<SharedData::Sync::SyncBaseline>();
    }
    catch (std::exception const& exception)
    {
        Log::error(
            "Sync baseline '{}' is damaged ({}), every delete of its pair is refused.",
            Paths::nativeToUtf8(file),
            Utility::errorMessage(exception)
        );
        return SharedData::Sync::SyncBaseline{};
    }
}

std::optional<SharedData::Sync::SyncBaseline> SyncBaselineStore::load(std::string const& pairKey) const
{
    std::scoped_lock lock{storeMutex()};
    return loadLocked(pairKey);
}

bool SyncBaselineStore::update(
    std::string const& pairKey,
    std::function<void(SharedData::Sync::SyncBaseline&)> const& change
) const
{
    std::scoped_lock lock{storeMutex()};
    const auto file = fileOf(directory_, pairKey);
    if (const auto existing = readJson(file);
        existing && *existing && (*existing)->value("pair", std::string{}) != pairKey)
    {
        Log::error("Sync baseline '{}' belongs to another pair, not replaced.", Paths::nativeToUtf8(file));
        return false;
    }

    auto baseline = loadLocked(pairKey).value_or(SharedData::Sync::SyncBaseline{});
    change(baseline);

    nlohmann::json json = baseline;
    json["pair"] = pairKey;
    json["version"] = formatVersion;

    std::error_code error{};
    std::filesystem::create_directories(directory_, error);
    if (error)
    {
        Log::error(
            "Cannot create the sync baseline directory '{}': {}",
            Paths::nativeToUtf8(directory_),
            Utility::errorMessage(error)
        );
        return false;
    }
    auto temporary = file;
    temporary += ".part";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        stream << json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!stream.flush())
        {
            Log::error("Cannot write the sync baseline '{}'.", Paths::nativeToUtf8(temporary));
            return false;
        }
    }
    if (auto moved = Utility::moveIntoPlace(temporary, file, /*replace=*/true); !moved)
    {
        Log::error(
            "Cannot replace the sync baseline '{}': {}", Paths::nativeToUtf8(file), Utility::errorMessage(moved.error())
        );
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
