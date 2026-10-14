#pragma once

#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/sync/sync_baseline.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Keeps the @ref SharedData::Sync::SyncBaseline of every sync pair, one JSON file per pair in a directory of
 *        its own, named by a hash of the pair's session and one of the pair key. The file also holds the key, so a
 *        hash collision reads as no baseline instead of another pair's records.
 *
 * Every access goes to the file under one mutex shared by all stores, so sync sessions and finished operations of
 * the same pair can update it from any thread without losing each other's changes.
 */
class SyncBaselineStore
{
  public:
    explicit SyncBaselineStore(std::filesystem::path directory);

    /**
     * @brief The store below the state directory of the program.
     */
    static std::filesystem::path defaultDirectory();

    /**
     * @brief Names a pair: the saved session, the local root and the remote root.
     */
    static std::string
    pairKey(std::string_view sessionName, Paths::LocalPath const& localRoot, Paths::RemotePath const& remoteRoot);

    /**
     * @brief The file of @p pairKey below @p directory.
     */
    static std::filesystem::path fileOf(std::filesystem::path const& directory, std::string_view pairKey);

    /**
     * @brief Removes the baselines of every pair whose session is not one of @p sessionNames, like a deleted or
     *        renamed session.
     * @return How many were removed.
     */
    std::size_t pruneAllBut(std::vector<std::string> const& sessionNames) const;

    /**
     * @brief The baseline of @p pairKey, nothing when the pair was never synchronized. A file that cannot be read
     *        counts as a pair with history but no records, which protects every delete.
     */
    std::optional<SharedData::Sync::SyncBaseline> load(std::string const& pairKey) const;

    /**
     * @brief Reads the baseline of @p pairKey (empty when there is none), lets @p change modify it and writes it
     *        back, which also creates the file of a pair synchronized for the first time.
     * @return false when it could not be written.
     */
    bool update(std::string const& pairKey, std::function<void(SharedData::Sync::SyncBaseline&)> const& change) const;

  private:
    std::optional<SharedData::Sync::SyncBaseline> loadLocked(std::string const& pairKey) const;

    std::filesystem::path directory_;
};
