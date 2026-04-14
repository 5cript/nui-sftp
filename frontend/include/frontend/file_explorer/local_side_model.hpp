#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <shared_data/file_operations/bulk_add_request.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
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
        FilePropertyDialog* filePropertyDialog
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

    bool isLeft() const override
    {
        return true;
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
    SideModel* remoteModel_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
    std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites_;
    std::function<void(std::vector<std::string>)> onFavoritesChanged_;
};