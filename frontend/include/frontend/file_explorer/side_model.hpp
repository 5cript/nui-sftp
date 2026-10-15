#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/file_explorer/overwrite_decisions.hpp>
#include <frontend/file_explorer/side_paths.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <persistence/state_holder.hpp>
#include <shared_data/opener_capabilities.hpp>
#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>
#include <log/log.hpp>

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/favorites_provider_interface.hpp>
#include <nui-file-explorer/context_menu_item.hpp>
#include <paths/paths.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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
class SideModel
    : public NuiFileExplorer::ISideModel
    , public NuiFileExplorer::IFavoritesProvider
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

    /**
     * @brief Sets the callback invoked whenever the favorites list changes. The callback
     *        receives the updated list as strings so the caller can persist it.
     *
     * @param callback Invoked on every add/remove.
     */
    void setOnFavoritesChanged(std::function<void(std::vector<std::string>)> callback);

    NuiFileExplorer::IFavoritesProvider* favoritesProvider() override
    {
        return this;
    }
    std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites() const override;
    void addFavorite(std::filesystem::path const& path) override;
    void removeFavorite(std::filesystem::path const& path) override;

  protected:
    // Design smell:
    virtual bool isComplete() const;

    /**
     * @brief Replaces the favorites without telling the callback, for the list the side starts with.
     */
    void setFavorites(std::vector<std::string> const& favorites);

    /**
     * @brief The context menu entries that add the one selected directory to the favorites or remove it, none for
     *        another selection.
     */
    std::vector<NuiFileExplorer::ContextMenuItem>
    favoriteMenuItems(std::vector<NuiFileExplorer::Item> const& selectedItems);

    /**
     * @brief The UTF-8 path of @p item on its side, refused for the ".." entry and for names that are no single
     *        path segment.
     */
    virtual std::expected<std::string, Paths::PathError> itemPathText(NuiFileExplorer::Item const& item) const = 0;

    /**
     * @brief The full path of each of @p items for the list of a confirmation, the listed path for an item whose
     *        name is no single path segment.
     */
    std::vector<ConfirmDialog::OpenOptions::ListElement> pathList(std::vector<NuiFileExplorer::Item> const& items) const;

    /**
     * @brief Shows the directory @p item stands for: the parent for "..", else the item's own path, the side may
     *        already be loading another directory.
     */
    void navigateInto(NuiFileExplorer::Item const& item);

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
     * @brief Texts of the questions a transfer asks.
     */
    struct TransferPrompt
    {
        std::string headerText{};
        /** @brief Question for one item, formatted with the destination name and @ref destinationDirectory. */
        std::string singleText{};
        /** @brief Question for several items, formatted with their count and @ref destinationDirectory. */
        std::string multipleText{};
        std::string destinationDirectory{};
        std::string overwriteHeaderText{};
        std::string overwriteText{};
    };

    /**
     * @brief One transfer entry per item of @p items: the source @p sourceOf gives for it, the destination with the
     *        source's name in @p destinationDirectory. Refused when an item names no source or its name cannot be
     *        used at the destination.
     */
    template <typename EntryT, typename SourceOfT, typename DirectoryT>
    static std::expected<std::vector<EntryT>, Paths::PathError> transferCandidates(
        std::vector<NuiFileExplorer::Item> const& items,
        SourceOfT const& sourceOf,
        DirectoryT const& destinationDirectory
    )
    {
        std::vector<EntryT> candidates;
        candidates.reserve(items.size());
        for (auto const& item : items)
        {
            auto source = sourceOf(item);
            if (!source)
                return std::unexpected(source.error());
            const auto name = source->filename();
            if (!name)
                return std::unexpected(
                    Paths::PathError{.reason = Paths::PathRejection::Empty, .input = FileExplorer::pathText(*source)}
                );
            auto destination = FileExplorer::childOf(destinationDirectory, *name);
            if (!destination)
                return std::unexpected(destination.error());
            EntryT entry{
                .source = *std::move(source),
                .destination = *std::move(destination),
                .sizeBytes = item.size,
                .isDirectory = item.isDirectory(),
            };
            if constexpr (requires { entry.mtime; })
            {
                entry.mtime = item.mtime;
                entry.mtimeNsec = item.mtimeNsec;
            }
            candidates.push_back(std::move(entry));
        }
        return candidates;
    }

    /**
     * @brief Asks whether to transfer @p candidates, checks which destinations exist, asks about each existing one
     *        and hands the accepted entries to @p onAccepted. Nothing is transferred when the check fails.
     */
    template <typename EntryT>
    void confirmTransfer(
        std::vector<EntryT> candidates,
        TransferPrompt prompt,
        std::function<void(std::vector<EntryT>)> onAccepted
    )
    {
        if (candidates.empty())
            return;
        const auto text = candidates.size() > 1
            ? fmt::format(fmt::runtime(prompt.multipleText), candidates.size(), prompt.destinationDirectory)
            : fmt::format(
                  fmt::runtime(prompt.singleText),
                  candidates.front().destination.filename()->view(),
                  prompt.destinationDirectory
              );
        std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
        listItems.reserve(candidates.size());
        std::ranges::transform(
            candidates,
            std::back_inserter(listItems),
            [](EntryT const& candidate)
            {
                return ConfirmDialog::OpenOptions::ListElement{.text = FileExplorer::pathText(candidate.source)};
            }
        );

        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = prompt.headerText,
            .text = text,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = std::move(listItems),
            .onClose = lifetime_.guarded(
                [this, candidates = std::move(candidates), prompt, onAccepted = std::move(onAccepted)](
                    std::optional<ConfirmDialog::Button> button
                ) mutable
                {
                    if (button != ConfirmDialog::Button::Yes)
                    {
                        Log::info("Transfer cancelled");
                        return;
                    }
                    checkDestinations(std::move(candidates), std::move(prompt), std::move(onAccepted));
                }
            ),
        });
    }

    /**
     * @brief Asks for a new name of @p item and renames it at @p from, next to itself. Refuses an item that names no
     *        path.
     */
    template <typename PathT>
    void promptRename(NuiFileExplorer::Item const& item, std::expected<PathT, Paths::PathError> const& from)
    {
        if (!from)
            return showPathRefused(
                language->get(isLeft() ? "localSideModel" : "remoteSideModel", "renameFailed"), from.error()
            );

        inputDialog_->open({
            .whatFor = language->get(isLeft() ? "localSideModel" : "remoteSideModel", "rename"),
            .prompt = fmt::format(
                fmt::runtime(language->get(isLeft() ? "localSideModel" : "remoteSideModel", "renamePrompt")),
                item.name
            ),
            .headerText = fmt::format(
                fmt::runtime(language->get(isLeft() ? "localSideModel" : "remoteSideModel", "renameWithItem")),
                item.name
            ),
            .initialValue = item.name,
            .isPassword = false,
            .onConfirm = lifetime_.guarded(
                [this, from = *from, currentName = item.name](std::optional<std::string> const& name)
                {
                    if (!name || name->empty() || *name == currentName)
                        return;
                    const auto directory = from.parent();
                    if (!directory)
                        return showPathRefused(
                            language->get(isLeft() ? "localSideModel" : "remoteSideModel", "renameFailed"),
                            Paths::PathError{
                                .reason = Paths::PathRejection::EscapesRoot, .input = FileExplorer::pathText(from)
                            }
                        );
                    const auto to = FileExplorer::childOf(*directory, std::string_view{*name});
                    if (!to)
                        return showInvalidItemName(to.error());

                    Log::info("Renaming '{}' to '{}'.", FileExplorer::pathText(from), *name);
                    renameItem(from, *to, false);
                }
            ),
        });
    }

    /**
     * @brief Asks for the name of a new item of @p type and hands its path in the listed directory to @p create.
     *        Refuses a name the side cannot use for a single item.
     */
    template <typename PathT>
    void promptNewItem(NuiFileExplorer::Item::Type type, std::function<void(PathT const&)> create)
    {
        if (type != NuiFileExplorer::Item::Type::Directory && type != NuiFileExplorer::Item::Type::Regular)
            return Log::error("New items of type {} are not supported.", static_cast<int>(type));

        const bool isDirectory = type == NuiFileExplorer::Item::Type::Directory;
        inputDialog_->open({
            .whatFor = language->get(
                isLeft() ? "localSideModel" : "remoteSideModel", isDirectory ? "newDirectory" : "newFile"
            ),
            .prompt = language->get(
                isLeft() ? "localSideModel" : "remoteSideModel",
                isDirectory ? "enterNewDirectoryName" : "enterNewFileName"
            ),
            .headerText = language->get(
                isLeft() ? "localSideModel" : "remoteSideModel", isDirectory ? "createNewDirectory" : "createNewFile"
            ),
            .isPassword = false,
            .onConfirm = lifetime_.guarded(
                [this, create = std::move(create)](std::optional<std::string> const& name)
                {
                    if (!name)
                        return;
                    const auto directory = FileExplorer::parsePath<PathT>(listedPath_.generic_string());
                    if (!directory)
                        return showPathRefused(
                            language->get(isLeft() ? "localSideModel" : "remoteSideModel", "createItemFailedTitle"),
                            directory.error()
                        );
                    const auto path = FileExplorer::childOf(*directory, std::string_view{*name});
                    if (!path)
                        return showInvalidItemName(path.error());
                    Log::info("Creating new item: {}", FileExplorer::pathText(*path));
                    create(*path);
                }
            ),
        });
    }

    /**
     * @brief Renames @p from to @p to, asks before replacing an existing item and retries with @p overwrite.
     */
    template <typename PathT>
    void renameItem(PathT const& from, PathT const& to, bool overwrite)
    {
        auto onRenamed = lifetime_.guarded(
            [this, from, to, overwrite](bool success, std::string const& info)
            {
                if (success)
                {
                    Log::info("Renamed '{}' to '{}'.", FileExplorer::pathText(from), FileExplorer::pathText(to));
                    return onRefresh();
                }
                if (info == "backend.rename.targetExists" && !overwrite)
                {
                    return confirmReplaceOnRename(
                        std::string{to.filename()->view()},
                        lifetime_.guarded(
                            [this, from, to]()
                            {
                                renameItem(from, to, true);
                            }
                        )
                    );
                }
                const auto reason = language->translate(info);
                Log::error(
                    "Failed to rename '{}' to '{}': {}", FileExplorer::pathText(from), FileExplorer::pathText(to), reason
                );
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get(isLeft() ? "localSideModel" : "remoteSideModel", "renameFailed"),
                    .text = fmt::format(
                        fmt::runtime(
                            language->get(isLeft() ? "localSideModel" : "remoteSideModel", "failedToRenameItem")
                        ),
                        FileExplorer::pathText(from),
                        FileExplorer::pathText(to),
                        reason
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
            }
        );
        if constexpr (std::is_same_v<PathT, Paths::LocalPath>)
            FileEngine::renameLocal(from, to, overwrite, std::move(onRenamed));
        else
            fileEngine_->rename(from, to, overwrite, std::move(onRenamed));
    }

  private:
    template <typename EntryT>
    void checkDestinations(
        std::vector<EntryT> candidates,
        TransferPrompt prompt,
        std::function<void(std::vector<EntryT>)> onAccepted
    )
    {
        using DestinationT = decltype(EntryT::destination);
        std::vector<DestinationT> destinations;
        destinations.reserve(candidates.size());
        std::ranges::transform(candidates, std::back_inserter(destinations), &EntryT::destination);

        auto onExists = lifetime_.guarded(
            [this, candidates = std::move(candidates), prompt = std::move(prompt), onAccepted = std::move(onAccepted)](
                std::vector<bool> exists, std::string const& info
            ) mutable
            {
                // An unknown destination is never taken for absent, the transfer stops instead.
                if (exists.size() != candidates.size())
                    return showExistenceCheckFailed(
                        exists.empty() ? language->translate(info)
                                       : fmt::format("{} of {} answers", exists.size(), candidates.size())
                    );
                askOverwrites(
                    std::make_shared<FileExplorer::OverwriteDecisions<EntryT>>(std::move(candidates), std::move(exists)),
                    std::make_shared<TransferPrompt const>(std::move(prompt)),
                    std::move(onAccepted)
                );
            }
        );
        if constexpr (std::is_same_v<DestinationT, Paths::LocalPath>)
            FileEngine::existsBatchLocal(destinations, std::move(onExists));
        else
            fileEngine_->existsBatchRemote(destinations, std::move(onExists));
    }

    template <typename EntryT>
    void askOverwrites(
        std::shared_ptr<FileExplorer::OverwriteDecisions<EntryT>> decisions,
        std::shared_ptr<TransferPrompt const> prompt,
        std::function<void(std::vector<EntryT>)> onAccepted
    )
    {
        const auto* question = decisions->nextQuestion();
        if (question == nullptr)
        {
            auto accepted = decisions->takeAccepted();
            if (!accepted.empty())
                onAccepted(std::move(accepted));
            return;
        }

        const auto destination = FileExplorer::pathText(question->destination);
        Log::info("The destination '{}' exists, asking whether to overwrite it.", destination);
        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = prompt->overwriteHeaderText,
            .text = prompt->overwriteText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                ConfirmDialog::Button::None | ConfirmDialog::Button::Cancel,
            .focusButton = ConfirmDialog::Button::No,
            .listItems = {{.text = destination}},
            .onClose = lifetime_.guarded(
                [this, decisions, prompt, onAccepted](std::optional<ConfirmDialog::Button> button)
                {
                    decisions->answer(overwriteAnswerOf(button));
                    askOverwrites(decisions, prompt, onAccepted);
                }
            ),
        });
    }

    void notifyFavoritesChanged();

    /**
     * @brief The icon URL the grid shows for @p entry.
     */
    std::string iconOf(SharedData::DirectoryEntry const& entry) const;

    /**
     * @brief The answer a button of the overwrite question stands for, closing the dialog cancels.
     */
    static FileExplorer::OverwriteAnswer overwriteAnswerOf(std::optional<ConfirmDialog::Button> button);

  protected:

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
    std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites_{
        std::make_shared<Nui::Observed<std::vector<std::filesystem::path>>>()
    };
    std::function<void(std::vector<std::string>)> onFavoritesChanged_{};
    /** @brief Guards callbacks that can arrive after the side model is gone, like RPC replies and dialog closes. */
    Utility::LifetimeToken lifetime_{};
};