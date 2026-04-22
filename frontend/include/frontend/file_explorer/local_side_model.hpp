#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <shared_data/file_operations/bulk_add_request.hpp>
#include <shared_data/opener_capabilities.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/places_provider_interface.hpp>
#include <nui-file-explorer/favorites_provider_interface.hpp>
#include <nui-file-explorer/path_suggestion_cache.hpp>

#include <nui/event_system/observed_value.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LocalSideModel
    : public SideModel
    , public NuiFileExplorer::IPlacesProvider
    , public NuiFileExplorer::IDrivesProvider
    , public NuiFileExplorer::IFavoritesProvider
{
  public:
    LocalSideModel(
        Persistence::UiOptions uiOptions,
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

    bool showRootEntry() const override;

    // --- IDrivesProvider ---
    NuiFileExplorer::IDrivesProvider* drivesProvider() override;
    void requestDrives(std::function<void(std::vector<PlaceEntry>)> callback) override;

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
     * and, on confirm, kicks off an upload of the selected items packed into a
     * single tar archive on the remote side. Backend wiring is still pending; for
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
    void onOpen(NuiFileExplorer::Item const& item, bool openWith);

    /**
     *  @brief Reveal @p item in the native file manager. Opens the parent directory with the item
     *         highlighted for files, or opens the directory itself for directories.
     */
    void onOpenInFileManager(NuiFileExplorer::Item const& item);

    bool isLeft() const override
    {
        return true;
    }
    SharedData::OpenerCapabilities openerCapabilities() const override
    {
        return openerCaps_;
    }
    void setRemoteModel(SideModel* model);
    bool isComplete() const override;
    void navigateTo(std::filesystem::path const& path) override;
    void onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries) override;

    void generatePathBoxSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    ) override;

  private:
    void enqueueRefresh(bool otherModel);

    void uploadItemsConfirmed(
        std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> uploadItems,
        std::shared_ptr<std::vector<bool>> existsResults,
        std::size_t index = 0,
        bool overwriteNever = false,
        bool overwriteAlways = false,
        std::shared_ptr<std::vector<SharedData::BulkAddEntry>> accepted = nullptr
    );

    void enqueueSingleUpload(
        NuiFileExplorer::Item const& remoteItem,
        NuiFileExplorer::Item const& localItem,
        bool allowOverwrite,
        bool insertRefresh
    );

  private:
    /**
     *  @brief Ask the backend once which external-open operations the platform can perform
     *         (xdg-desktop-portal probe on Linux). The result is stashed in @ref openerCaps_
     *         and drives the context menu's disabled-state. On failure a one-shot warning
     *         dialog is opened via @ref confirmDialog_.
     */
    void probeOpenerCapabilities();

    SideModel* remoteModel_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
    std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites_;
    std::function<void(std::vector<std::string>)> onFavoritesChanged_;
    SharedData::OpenerCapabilities openerCaps_{};
};