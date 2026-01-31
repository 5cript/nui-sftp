#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <persistence/state_holder.hpp>

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
        FilePropertyDialog* filePropertyDialog
    )
        : uiOptions_{std::move(uiOptions)}
        , confirmDialog_{confirmDialog}
        , inputDialog_{inputDialog}
        , filePropertyDialog_{filePropertyDialog}
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

  protected:
    // Design smell:
    virtual bool isComplete() const;

    virtual void onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries);

  protected:
    std::shared_ptr<FileEngine> fileEngine_{nullptr};
    Persistence::UiOptions uiOptions_;
    ConfirmDialog* confirmDialog_;
    InputDialog* inputDialog_;
    FilePropertyDialog* filePropertyDialog_;
    std::vector<NuiFileExplorer::Item> items_{};
    OperationQueue* operationQueue_{nullptr};
    Nui::Observed<std::filesystem::path> currentPath_{};
    std::filesystem::path preNavigatePath_{};
    std::function<void(bool, bool)> refreshCallback_{nullptr};
    bool reapplySelectionOnce_{false};
    std::string dropMetadata_{};
};