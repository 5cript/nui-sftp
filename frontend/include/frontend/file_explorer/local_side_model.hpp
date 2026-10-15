#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/opener_capabilities.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
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
#include <string>
#include <vector>

class RemoteSideModel;

class LocalSideModel
    : public SideModel
    , public NuiFileExplorer::IPlacesProvider
    , public NuiFileExplorer::IDrivesProvider
{
  public:
    LocalSideModel(
        Persistence::UiOptions uiOptions,
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

    bool showRootEntry() const override;

    // --- IDrivesProvider ---
    NuiFileExplorer::IDrivesProvider* drivesProvider() override;
    void requestDrives(std::function<void(std::vector<PlaceEntry>)> callback) override;

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
    void onError(NuiFileExplorer::Error const& error) override;
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
    void setRemoteModel(RemoteSideModel* model);

    /**
     * @brief The directory the shown items were listed from, an error before the first listing.
     */
    std::expected<Paths::LocalPath, Paths::PathError> currentLocalPath() const;
    bool isComplete() const override;
    void navigateTo(std::filesystem::path const& path) override;
    std::optional<std::filesystem::path> parentOf(std::filesystem::path const& directory) const override;

    void generatePathBoxSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    ) override;

  private:
    /**
     * @brief Where @p item is: a listed item below the directory it was listed from, a dropped one at its own
     *        absolute path. A listed name that is no single path segment, like "..", names no item.
     */
    std::expected<Paths::LocalPath, Paths::PathError> localPathOf(NuiFileExplorer::Item const& item) const;

    std::expected<std::string, Paths::PathError> itemPathText(NuiFileExplorer::Item const& item) const override;

    /**
     * @brief Queues the uploads the user accepted, a single file as a transfer of its own.
     */
    void enqueueUploads(std::vector<SharedData::BulkUploadEntry> accepted);

    void enqueueSingleUpload(SharedData::BulkUploadEntry const& entry, bool allowOverwrite, bool insertRefresh);

    /**
     * @brief Deletes @p targets through the backend at once, each item by itself, and refreshes the listing.
     */
    void deleteFilesDirectly(std::vector<Paths::LocalPath> const& targets);

    /**
     * @brief Shows why deleting local items failed.
     */
    void showDeleteFailed(std::string const& reason);

  private:
    /**
     *  @brief Ask the backend once which external-open operations the platform can perform
     *         (xdg-desktop-portal probe on Linux). The result is stashed in @ref openerCaps_
     *         and drives the context menu's disabled-state. On failure a one-shot warning
     *         dialog is opened via @ref confirmDialog_.
     */
    void probeOpenerCapabilities();

    RemoteSideModel* remoteModel_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
    SharedData::OpenerCapabilities openerCaps_{};
};