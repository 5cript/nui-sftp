#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <persistence/state_holder.hpp>
#include <shared_data/opener_capabilities.hpp>

#include <nui-file-explorer/side_model_interface.hpp>

#include <source_location>

// This should be an assertion, but assert failures are hard to debug in the frontend,
// log messages and noop is much easier to deal with.
#define CHECK_COMPLETE_RET(RETX) \
    do \
    { \
        if (!isComplete()) \
        { \
            auto loc = std::source_location::current(); \
            Log::error( \
                "SideModel is used before the setup is complete. ({}:{} {})", \
                loc.file_name(), \
                loc.line(), \
                loc.function_name() \
            ); \
            return RETX; \
        } \
    } while (false)

// This should be an assertion, but assert failures are hard to debug in the frontend,
// log messages and noop is much easier to deal with.
#define CHECK_COMPLETE() CHECK_COMPLETE_RET()

// Incomplete class, provides base functionality
class SideModel : public NuiFileExplorer::ISideModel
{
  public:
    SideModel(
        Persistence::UiOptions uiOptions,
        ConfirmDialog* confirmDialog,
        InputDialog* inputDialog,
        FilePropertyDialog* filePropertyDialog,
        ArchiveTransferDialog* archiveTransferDialog
    )
        : uiOptions_{std::move(uiOptions)}
        , confirmDialog_{confirmDialog}
        , inputDialog_{inputDialog}
        , filePropertyDialog_{filePropertyDialog}
        , archiveTransferDialog_{archiveTransferDialog}
    {}
    ~SideModel() override = default;
    SideModel(const SideModel&) = delete;
    SideModel& operator=(const SideModel&) = delete;
    SideModel(SideModel&&) = default;
    SideModel& operator=(SideModel&&) = default;

    void engine(std::shared_ptr<FileEngine> fileEngine);
    std::shared_ptr<FileEngine> engine();

    void operationQueue(OperationQueue* operationQueue);
    OperationQueue* operationQueue();

    void setItemUpdateFunction(std::function<void(bool, bool)> doUpdate) override;
    const std::vector<NuiFileExplorer::Item>& items() const override;
    void onPathChange(std::filesystem::path const& path) override
    {
        navigateTo(path);
    }
    void onRefresh() override
    {
        reapplySelectionOnce_ = true;
        navigateTo(currentPath_.value());
    }
    Nui::Observed<std::filesystem::path> const& currentPath() const override
    {
        return currentPath_;
    }
    void goBack() override
    {
        if (preNavigatePath_.empty())
            return;
        navigateTo(preNavigatePath_);
    }

    std::string dropMetadata() const override
    {
        return dropMetadata_;
    }

    void dropMetadata(std::string const& value) override
    {
        dropMetadata_ = value;
    }

    /**
     *  @brief Current snapshot of the platform opener probe result. Defaults to all-capable;
     *         LocalSideModel overrides this once the backend probe completes. RemoteSideModel
     *         reads it through its localModel_ pointer so remote "Open" / "Open With" / watch
     *         variants can be grayed out in lockstep.
     */
    virtual SharedData::OpenerCapabilities openerCapabilities() const
    {
        return {};
    }

  protected:
    // Design smell:
    virtual bool isComplete() const;

    /**
     * @brief A new name is a single path segment: not empty, not "." or "..", without '/', '\\' or NUL.
     */
    static bool isValidItemName(std::string_view name);

    /**
     * @brief Tells the user @p name cannot be used as a name.
     */
    void showInvalidItemName(std::string const& name);

    /**
     * @brief Tells the user an action was refused because of the path in @p error.
     */
    void showPathRefused(std::string const& headerText, Paths::PathError const& error);

    /**
     * @brief Tells the user a transfer stopped because the destinations could not be checked, @p reason says why.
     */
    void showExistenceCheckFailed(std::string const& reason);

    /**
     * @brief Asks whether the existing item @p name may be replaced, runs @p onReplace when the user agrees.
     */
    void confirmReplaceOnRename(std::string const& name, std::function<void()> onReplace);

    /**
     * @brief The directory has a parent to offer as "..".
     */
    virtual bool hasParent(std::filesystem::path const& directory) const
    {
        return directory.has_relative_path();
    }

    /**
     * @brief Shows the listing of @p directory. A reply for a directory the side has navigated away from is dropped,
     *        so no item carries the path of another directory.
     */
    virtual void onDirectoryListing(
        std::filesystem::path const& directory,
        std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries,
        std::string const& reason = {}
    );

  protected:
    std::shared_ptr<FileEngine> fileEngine_{nullptr};
    Persistence::UiOptions uiOptions_;
    ConfirmDialog* confirmDialog_;
    InputDialog* inputDialog_;
    FilePropertyDialog* filePropertyDialog_;
    ArchiveTransferDialog* archiveTransferDialog_;
    std::vector<NuiFileExplorer::Item> items_{};
    OperationQueue* operationQueue_{nullptr};
    Nui::Observed<std::filesystem::path> currentPath_{};
    /** @brief The directory the shown items were listed from, currentPath_ runs ahead while a listing loads. */
    std::filesystem::path listedPath_{};
    std::filesystem::path preNavigatePath_{};
    std::function<void(bool, bool)> refreshCallback_{nullptr};
    bool reapplySelectionOnce_{false};
    std::string dropMetadata_{};
};