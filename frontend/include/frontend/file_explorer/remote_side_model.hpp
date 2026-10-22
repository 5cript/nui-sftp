#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/places_provider_interface.hpp>
#include <nui-file-explorer/path_suggestion_cache.hpp>

#include <nui/event_system/observed_value.hpp>
#include <paths/paths.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class FileTrackingPanel;
class LocalSideModel;

class RemoteSideModel
    : public SideModel
    , public NuiFileExplorer::IPlacesProvider
{
  public:
    RemoteSideModel(
        Persistence::UiOptions uiOptions,
        std::vector<std::string> initialFavorites,
        ConfirmDialog* confirmDialog,
        InputDialog* inputDialog,
        FilePropertyDialog* filePropertyDialog,
        ArchiveTransferDialog* archiveTransferDialog
    );

    // --- IPlacesProvider ---
    NuiFileExplorer::IPlacesProvider* placesProvider() override
    {
        return this;
    }
    void requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback) override;

    /**
     * @brief Sets the remote home directory the default places are derived from.
     *
     * @param home The remote home directory.
     * @param readUserDirectories Whether the places come from the user-dirs.dirs file of the server when it has one.
     */
    void setRemoteHome(std::filesystem::path home, bool readUserDirectories);

    void onActivateItem(NuiFileExplorer::Item const& item) override;
    void onNewItem(NuiFileExplorer::Item::Type type) override;
    void onDelete(std::vector<NuiFileExplorer::Item> const& items) override;
    void onTransfer(std::vector<NuiFileExplorer::Item> const& items, std::optional<std::string> const& subDir) override;

    /**
     * @brief Asks for the name, codec and level of an archive and queues a download of @p items packed into one
     *        tar archive in the listed local directory.
     */
    void onTransferAsArchive(std::vector<NuiFileExplorer::Item> const& items);
    std::vector<NuiFileExplorer::ContextMenuItem>
    contextMenuItems(std::vector<NuiFileExplorer::Item> const& selectedItems) override;
    void onDropExternal(
        std::vector<NuiFileExplorer::Item> const& items,
        std::optional<std::string> const& subDir,
        bool issueWebkitWarning
    ) override;
    void onRename(NuiFileExplorer::Item const& item) override;
    void onProperties(NuiFileExplorer::Item const& item) override;
    void onError(NuiFileExplorer::Error const& error) override;

    bool isLeft() const override
    {
        return false;
    }
    void navigateTo(std::filesystem::path const& path) override;
    void setLocalModel(LocalSideModel* model);
    std::optional<std::filesystem::path> parentOf(std::filesystem::path const& directory) const override;

    /**
     * @brief The directory the shown items were listed from, an error before the first listing.
     */
    std::expected<Paths::RemotePath, Paths::PathError> currentRemotePath() const;
    void setFileTracking(FileTrackingPanel* fileTracking);
    bool isComplete() const override;

    void generatePathBoxSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    ) override;

  private:
    /**
     * @brief Where @p item is on the server, below the directory it was listed from. A listed name that is no
     *        single path segment, like "..", names no item.
     */
    std::expected<Paths::RemotePath, Paths::PathError> remotePathOf(NuiFileExplorer::Item const& item) const;

    std::expected<std::string, Paths::PathError> itemPathText(NuiFileExplorer::Item const& item) const override;

    /**
     * @brief Queues the downloads the user accepted, a single file as a transfer of its own.
     */
    void enqueueDownloads(std::vector<SharedData::BulkDownloadEntry> accepted);

    void enqueueSingleDownload(SharedData::BulkDownloadEntry const& entry, bool allowOverwrite, bool insertRefresh);

    void askNonEmptyDirectoryDeletions(
        std::vector<Paths::RemotePath> confirmedToDelete,
        std::vector<Paths::RemotePath> nonEmpties,
        std::vector<Paths::RemotePath> filesAndEmptyDirs,
        std::size_t currentNonEmpty
    );

    void
    enqueueDeletes(std::vector<Paths::RemotePath> nonEmpties, std::vector<Paths::RemotePath> filesAndEmptyDirs);

    /**
     * @brief Has the file tracker download @p item into a directory of its own and opens it, with @p watch the
     *        tracker also uploads it again when it changes.
     */
    void onDownloadAndOpen(NuiFileExplorer::Item const& item, bool openWith, bool watch);
    /**
     * @brief What happens when the user decides not to open the download @p localPath of the instance
     *        @p instanceId: the download is removed and its watch ends, see FileTrackingPanel::discard.
     */
    std::function<void()> discardWhenDeclined(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath);

  private:
    void placesFromHomeListing();
    void finishPlacesRequests();
    /**
     * @brief Requests the places again for pending requests that were made for a previous home.
     */
    void retryPlacesRequests();
    std::vector<PlaceEntry> defaultPlaceEntries() const;

  private:
    std::filesystem::path remoteHome_;
    bool readUserDirectories_{true};
    /**
     * @brief Kind and path of the default places besides home, determined once per home.
     */
    std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> remotePlaces_;
    /**
     * @brief Requests waiting for the places request in flight.
     */
    std::vector<std::function<void(std::vector<PlaceEntry>)>> pendingPlacesRequests_;
    LocalSideModel* localModel_{nullptr};
    FileTrackingPanel* fileTracking_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
};