#include <frontend/session_components/file_explorer_panel.hpp>

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <persistence/state_holder.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <nui-file-explorer/file_grid.hpp>

#include <fmt/format.h>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

namespace
{
    NuiFileExplorer::FileGrid buildFileGrid(FileExplorerPanel::Params& params)
    {
        auto* filePropertyDialog = params.filePropertyDialog;
        auto* confirmDialog = params.confirmDialog;
        auto* inputDialog = params.inputDialog;
        auto* archiveTransferDialog = params.archiveTransferDialog;
        auto const& uiOptions = params.uiOptions;
        auto* stateHolder = params.stateHolder;

        if (std::holds_alternative<Persistence::SshSessionOptions>(params.engineOptions.engine))
        {
            return {
                {
                    .pathBarOnTop = uiOptions.fileGridPathBarOnTop,
                    .showHiddenFiles = uiOptions.showHiddenFilesLocally,
                    .onShowHiddenFilesChanged = [stateHolder](bool value) {
                        stateHolder->loadModifySave([value](Persistence::State& state) {
                            state.uiOptions.showHiddenFilesLocally = value;
                        });
                    },
                    .pageSize = uiOptions.fileGridPageSize,
                },
                {
                    .pathBarOnTop = uiOptions.fileGridPathBarOnTop,
                    .showHiddenFiles = uiOptions.showHiddenFilesRemotely,
                    .onShowHiddenFilesChanged = [stateHolder](bool value) {
                        stateHolder->loadModifySave([value](Persistence::State& state) {
                            state.uiOptions.showHiddenFilesRemotely = value;
                        });
                    },
                    .pageSize = uiOptions.fileGridPageSize,
                },
                std::make_unique<LocalSideModel>(
                    uiOptions, confirmDialog, inputDialog, filePropertyDialog, archiveTransferDialog
                ),
                std::make_unique<RemoteSideModel>(
                    uiOptions,
                    std::get<Persistence::SshSessionOptions>(params.engineOptions.engine).remoteFavorites,
                    confirmDialog,
                    inputDialog,
                    filePropertyDialog,
                    archiveTransferDialog
                ),
            };
        }

        // Local-only (executing-engine) sessions: left side only, no remote.
        return {
            {
                .pathBarOnTop = uiOptions.fileGridPathBarOnTop,
                .showHiddenFiles = uiOptions.showHiddenFilesLocally,
                .onShowHiddenFilesChanged = [stateHolder](bool value) {
                    stateHolder->loadModifySave([value](Persistence::State& state) {
                        state.uiOptions.showHiddenFilesLocally = value;
                    });
                },
                .pageSize = uiOptions.fileGridPageSize,
            },
            std::make_unique<LocalSideModel>(
                uiOptions, confirmDialog, inputDialog, filePropertyDialog, archiveTransferDialog
            ),
        };
    }
}

struct FileExplorerPanel::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;
    InputDialog* inputDialog;
    FilePropertyDialog* filePropertyDialog;
    ArchiveTransferDialog* archiveTransferDialog;
    std::string sessionName;
    Persistence::UiOptions uiOptions;
    Persistence::SessionOptions engineOptions;
    Nui::Observed<bool>* isInLostConnectionState;

    NuiFileExplorer::FileGrid fileGrid;
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> fileExplorerElement{};

    explicit Implementation(Params&& params)
        : stateHolder{params.stateHolder}
        , events{params.events}
        , confirmDialog{params.confirmDialog}
        , inputDialog{params.inputDialog}
        , filePropertyDialog{params.filePropertyDialog}
        , archiveTransferDialog{params.archiveTransferDialog}
        , sessionName{std::move(params.sessionName)}
        , uiOptions{params.uiOptions}
        , engineOptions{std::move(params.engineOptions)}
        , isInLostConnectionState{params.isInLostConnectionState}
        , fileGrid{buildFileGrid(params)}
    {}
};

FileExplorerPanel::FileExplorerPanel(Params params)
    : impl_{std::make_unique<Implementation>(std::move(params))}
{}
FileExplorerPanel::~FileExplorerPanel() = default;
FileExplorerPanel::FileExplorerPanel(FileExplorerPanel&&) = default;
FileExplorerPanel& FileExplorerPanel::operator=(FileExplorerPanel&&) = default;

void FileExplorerPanel::setup()
{
    // Persist favorites changes through StateHolder.  Local-side favorites
    // live in uiOptions; remote-side favorites live inside the SSH session's
    // engine config, so the remote callback has to look up the session by
    // name at persist time.
    static_cast<LocalSideModel*>(&impl_->fileGrid.leftModel())
        ->setOnFavoritesChanged(
            [stateHolder = impl_->stateHolder](std::vector<std::string> favs)
            {
                stateHolder->loadModifySave(
                    [favs = std::move(favs)](Persistence::State& state)
                    {
                        state.uiOptions.localFavorites = favs;
                    }
                );
            }
        );
    if (auto* remote = static_cast<RemoteSideModel*>(impl_->fileGrid.rightModel()); remote)
    {
        remote->setOnFavoritesChanged(
            [stateHolder = impl_->stateHolder, sessionName = impl_->sessionName](
                std::vector<std::string> favs)
            {
                stateHolder->loadModifySave(
                    [favs = std::move(favs), sessionName](Persistence::State& state)
                    {
                        const auto iter = state.sessions.find(sessionName);
                        if (iter == state.sessions.end())
                            return;
                        if (auto* ssh = std::get_if<Persistence::SshSessionOptions>(&iter->second.engine))
                            ssh->remoteFavorites = favs;
                    }
                );
            }
        );
    }

    impl_->fileGrid.onError(
        [confirmDialog = impl_->confirmDialog](auto const& message)
        {
            Log::error("File grid error: {}", message);
            confirmDialog->open({
                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                .headerText = "File Grid Error",
                .text = message,
                .buttons = ConfirmDialog::Button::Ok,
                .neverShowAgainId = "fileGridError",
            });
        }
    );

    if (auto* remote = remoteSideModel(); remote)
    {
        remote->setItemUpdateFunction(
            [this](bool sorted, bool reapplySelection)
            {
                if (auto* side = remoteFileGridSide(); side)
                    side->updateItems(sorted, reapplySelection);
            }
        );
    }
    localSideModel().setItemUpdateFunction(
        [this](bool sorted, bool reapplySelection)
        {
            localFileGridSide().updateItems(sorted, reapplySelection);
        }
    );
    if (auto* remote = remoteSideModel(); remote)
    {
        remote->setLocalModel(&localSideModel());
        localSideModel().setRemoteModel(remote);
    }
    else
    {
        localSideModel().setRemoteModel(nullptr);
    }
}

void FileExplorerPanel::setOnSynchronize(
    std::function<void(std::filesystem::path, std::filesystem::path)> onSync
)
{
    localFileGridSide().setOnSynchronize(onSync);
    if (auto* remote = remoteFileGridSide(); remote)
        remote->setOnSynchronize(std::move(onSync));
}

Nui::ElementRenderer FileExplorerPanel::makeFileExplorerElement()
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    // Per-panel view blocker — SFTP-bound widgets dim themselves during a
    // reconnect cycle so the user can't act on stale remote state.
    // Terminal panels are deliberately not blocked (local shells keep
    // working; primary SSH terminals are locked at the engine level).
    return div{
        style = "width: 100%; height: 100%; position: relative; display: block",
    }(
        impl_->fileGrid(),
        div{
            style = observe(*impl_->isInLostConnectionState).generate([this](){
                return fmt::format("display: {};", impl_->isInLostConnectionState->value() ? "block" : "none");
            }),
            class_ = "session-panel-blocker",
        }()
    );
    // clang-format on
}

void FileExplorerPanel::onDrop(
    bool isLocalSide,
    std::vector<SharedData::DirectoryEntry> entries,
    std::optional<std::string> const& subdir
)
{
    if (isLocalSide)
    {
        // confirm dialog: not implemented
        Log::warn("Dropping files on local side is not implemented.");
        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "dropNotImplementedTitle"),
            .text = language->get("sessionFrontend", "dropNotImplementedText"),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "dropNotImplemented",
        });
        return;
    }

    std::vector<NuiFileExplorer::Item> items;
    items.reserve(entries.size());
    std::transform(
        std::make_move_iterator(begin(entries)),
        std::make_move_iterator(end(entries)),
        std::back_inserter(items),
        [](SharedData::DirectoryEntry const& entry)
        {
            return NuiFileExplorer::Item{entry};
        }
    );

    localSideModel().onTransfer(items, subdir);
}

void FileExplorerPanel::dropLayoutMetadata(std::string const& sessionLayoutId)
{
    impl_->fileGrid.leftModel().dropMetadata(sessionLayoutId);
    if (impl_->fileGrid.rightModel())
        impl_->fileGrid.rightModel()->dropMetadata(sessionLayoutId);
}

NuiFileExplorer::FileGrid& FileExplorerPanel::fileGrid()
{
    return impl_->fileGrid;
}

NuiFileExplorer::Side& FileExplorerPanel::localFileGridSide()
{
    return impl_->fileGrid.leftSide();
}

NuiFileExplorer::Side* FileExplorerPanel::remoteFileGridSide()
{
    return impl_->fileGrid.rightSide();
}

LocalSideModel& FileExplorerPanel::localSideModel()
{
    return static_cast<LocalSideModel&>(localFileGridSide().model());
}

RemoteSideModel* FileExplorerPanel::remoteSideModel()
{
    if (!remoteFileGridSide())
        return nullptr;
    return static_cast<RemoteSideModel*>(&remoteFileGridSide()->model());
}

Nui::Observed<std::shared_ptr<Nui::Dom::Element>>&
FileExplorerPanel::elementObservable()
{
    return impl_->fileExplorerElement;
}
