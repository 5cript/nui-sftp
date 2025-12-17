#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>

#include <source_location>

// This should be an assertion, but assert failures are hard to debug in the frontend,
// log messages and noop is much easier to deal with.
#define CHECK_COMPLETE() \
    do \
    { \
        if (!isComplete()) \
        { \
            auto loc = std::source_location::current(); \
            Log::error( \
                "RemoteSideModel is used before the setup is complete. ({}:{} {})", \
                loc.file_name(), \
                loc.line(), \
                loc.function_name()); \
            return; \
        } \
    } while (false)

// Incomplete class, provides base functionality
class SideModel : public NuiFileExplorer::ISideModel
{
  public:
    SideModel(Persistence::UiOptions uiOptions, ConfirmDialog* confirmDialog, InputDialog* inputDialog)
        : uiOptions_{std::move(uiOptions)}
        , confirmDialog_{confirmDialog}
        , inputDialog_{inputDialog}
    {}
    ~SideModel() override = default;
    SideModel(const SideModel&) = delete;
    SideModel& operator=(const SideModel&) = delete;
    SideModel(SideModel&&) = default;
    SideModel& operator=(SideModel&&) = default;

    void engine(std::unique_ptr<FileEngine> fileEngine);
    FileEngine* engine();

    void operationQueue(OperationQueue* operationQueue);
    OperationQueue* operationQueue();

    void setItemUpdateFunction(std::function<void(bool)> doUpdate) override;

  protected:
    // Design smell:
    bool isComplete() const;

  protected:
    Persistence::UiOptions uiOptions_;
    ConfirmDialog* confirmDialog_;
    InputDialog* inputDialog_;
    std::vector<NuiFileExplorer::Item> items_{};
    std::unique_ptr<FileEngine> fileEngine_{nullptr};
    OperationQueue* operationQueue_{nullptr};
    std::filesystem::path currentPath_{};
    std::filesystem::path preNavigatePath_{};
    std::function<void(bool)> refreshCallback_{nullptr};
};