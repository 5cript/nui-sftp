#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/favorites_provider_interface.hpp>
#include <nui-file-explorer/places_provider_interface.hpp>
#include <nui-file-explorer/path_suggestion_cache.hpp>

#include <nui/event_system/observed_value.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class FileTrackingPanel;

class RemoteSideModel
    : public SideModel
    , public NuiFileExplorer::IPlacesProvider
    , public NuiFileExplorer::IFavoritesProvider
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

    /**
     * @brief Sets the callback invoked whenever the favorites list changes. The callback
     *        receives the updated list as strings so the caller can persist it.
     *
     * @param callback Invoked on every add/remove.
     */
    void setOnFavoritesChanged(std::function<void(std::vector<std::string>)> callback);

    // --- IPlacesProvider ---
    NuiFileExplorer::IPlacesProvider* placesProvider() override
    {
        return this;
    }
    void requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback) override;

    void setRemoteUsername(std::string username);

    // --- IFavoritesProvider ---
    NuiFileExplorer::IFavoritesProvider* favoritesProvider() override
    {
        return this;
    }
    std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites() const override;
    void addFavorite(std::filesystem::path const& path) override;
    void removeFavorite(std::filesystem::path const& path) override;

    void onActivateItem(NuiFileExplorer::Item const& item) override;
    void onNewItem(NuiFileExplorer::Item::Type type) override;
    void onDelete(std::vector<NuiFileExplorer::Item> const& items) override;
    void onTransfer(std::vector<NuiFileExplorer::Item> const& items, std::optional<std::string> const& subDir) override;

    /**
     * @brief "Transfer as Archive" entry point: opens the archive-transfer dialog
     * and, on confirm, kicks off a download of the selected items packed into a
     * single tar archive on the local side. Backend wiring is still pending; for
     * now the onConfirm just logs the intent.
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
    void onError(std::string const& error) override;

    bool isLeft() const override
    {
        return false;
    }
    void navigateTo(std::filesystem::path const& path) override;
    void setLocalModel(SideModel* model);
    void setFileTracking(FileTrackingPanel* fileTracking);
    bool isComplete() const override;

    void generatePathBoxSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    ) override;

  private:
    void enqueueRefresh(bool otherModel);

    void downloadItemsConfirmed(
        std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> downloadItems,
        std::shared_ptr<std::vector<bool>> existsResults,
        std::size_t index = 0,
        bool overwriteNever = false,
        bool overwriteAlways = false,
        std::shared_ptr<std::vector<SharedData::BulkAddEntry>> accepted = nullptr
    );

    void enqueueSingleDownload(
        NuiFileExplorer::Item const& remoteItem,
        NuiFileExplorer::Item const& localItem,
        bool allowOverwrite,
        bool insertRefresh
    );

    void askNonEmptyDirectoryDeletions(
        std::vector<std::filesystem::path> confirmedToDelete,
        std::vector<std::filesystem::path> nonEmpties,
        std::vector<std::filesystem::path> filesAndEmptyDirs,
        std::size_t currentNonEmpty
    );

    void
    enqueueDeletes(std::vector<std::filesystem::path> nonEmpties, std::vector<std::filesystem::path> filesAndEmptyDirs);

    void continueDeletion();
    void onDownloadAndOpen(NuiFileExplorer::Item const& item, bool openWith);
    void onWatchDownloadAndOpen(NuiFileExplorer::Item const& item, bool openWith);
    void onFileTrackingInstanceCreated(
        NuiFileExplorer::Item const& item,
        bool openWith,
        bool synchronize,
        Ids::InstanceId const& instanceId,
        std::string const& instanceDirStr
    );
    void onFileDownloadComplete(
        Ids::InstanceId const& instanceId,
        std::filesystem::path const& instanceDir,
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        bool openWith,
        bool synchronize,
        bool success
    );
    void onFileWatchAdded(
        Ids::InstanceId const& instanceId,
        std::filesystem::path const& instanceDir,
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        bool openWith
    );

  private:
    std::string remoteUsername_;
    SideModel* localModel_{nullptr};
    FileTrackingPanel* fileTracking_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
    std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites_;
    std::function<void(std::vector<std::string>)> onFavoritesChanged_;
};