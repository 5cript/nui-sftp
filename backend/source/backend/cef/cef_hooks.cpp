#include <backend/cef/cef_hooks.hpp>
#include <backend/dropped_entry.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>

#include <nui/cef/client_hooks.hpp>

#include <include/cef_context_menu_handler.h>
#include <include/cef_drag_handler.h>
#include <include/cef_request_handler.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    /// Main::show navigates there.
    constexpr std::string_view appOrigin = "nui://app.example/";

    /**
     * @brief Paths of the files of the last drag that entered the window.
     */
    class DraggedFiles
    {
      public:
        void remember(std::vector<std::filesystem::path> paths)
        {
            std::scoped_lock lock{mutex_};
            paths_ = std::move(paths);
        }

        /**
         * @brief Takes the remembered paths when their file names are @p names, the names the page got.
         * @details The drag handler only learns about drags entering the window, not about drops, so the names keep a
         *          claim from taking the files of an earlier drag that never dropped.
         */
        std::vector<std::filesystem::path> take(std::vector<std::string> names)
        {
            std::scoped_lock lock{mutex_};
            auto paths = std::move(paths_);
            paths_.clear();

            std::vector<std::string> pathNames;
            pathNames.reserve(paths.size());
            for (auto const& path : paths)
                pathNames.push_back(Paths::nativeToUtf8(path.filename()));
            std::ranges::sort(pathNames);
            std::ranges::sort(names);
            if (pathNames != names)
                return {};
            return paths;
        }

      private:
        std::mutex mutex_{};
        std::vector<std::filesystem::path> paths_{};
    };

    class ContextMenuHandler : public CefContextMenuHandler
    {
      public:
        void OnBeforeContextMenu(
            CefRefPtr<CefBrowser>,
            CefRefPtr<CefFrame>,
            CefRefPtr<CefContextMenuParams>,
            CefRefPtr<CefMenuModel> model
        ) override
        {
            for (auto const command :
                 {MENU_ID_BACK,
                  MENU_ID_FORWARD,
                  MENU_ID_RELOAD,
                  MENU_ID_RELOAD_NOCACHE,
                  MENU_ID_STOPLOAD,
                  MENU_ID_PRINT,
                  MENU_ID_VIEW_SOURCE})
            {
                model->Remove(command);
            }
            removeStraySeparators(*model);
        }

      private:
        static bool isSeparator(CefMenuModel& model, std::size_t index)
        {
            return model.GetTypeAt(index) == MENUITEMTYPE_SEPARATOR;
        }

        /// Leading, trailing and doubled separators are left over from the removed entries.
        static void removeStraySeparators(CefMenuModel& model)
        {
            for (std::size_t index = 0u; index < model.GetCount();)
            {
                const bool stray = isSeparator(model, index) &&
                    (index == 0u || index + 1u == model.GetCount() || isSeparator(model, index + 1u));
                if (stray)
                    model.RemoveAt(index);
                else
                    ++index;
            }
        }

        IMPLEMENT_REFCOUNTING(ContextMenuHandler);
    };

    class DragHandler : public CefDragHandler
    {
      public:
        explicit DragHandler(std::shared_ptr<DraggedFiles> files)
            : files_{std::move(files)}
        {}

        bool OnDragEnter(CefRefPtr<CefBrowser>, CefRefPtr<CefDragData> dragData, DragOperationsMask) override
        {
            std::vector<CefString> cefPaths;
            dragData->GetFilePaths(cefPaths);

            std::vector<std::filesystem::path> paths;
            paths.reserve(cefPaths.size());
            for (auto const& path : cefPaths)
                paths.emplace_back(Paths::nativeFromUtf8(path.ToString()));
            files_->remember(std::move(paths));
            return false;
        }

      private:
        std::shared_ptr<DraggedFiles> files_;
        IMPLEMENT_REFCOUNTING(DragHandler);
    };

    class RequestHandler : public CefRequestHandler
    {
      public:
        bool OnBeforeBrowse(
            CefRefPtr<CefBrowser>,
            CefRefPtr<CefFrame> frame,
            CefRefPtr<CefRequest> request,
            bool,
            bool
        ) override
        {
            if (!frame->IsMain())
                return false;

            // The app is one document: once it is loaded, a navigation, a reload too, would lose its RPC setup.
            const auto url = request->GetURL().ToString();
            const bool appLoaded = frame->GetURL().ToString().starts_with(appOrigin);
            if (!appLoaded && (url.starts_with(appOrigin) || url == "about:blank"))
                return false;

            Log::warn("Blocked a navigation of the window to '{}'.", url);
            return true;
        }

        bool OnOpenURLFromTab(
            CefRefPtr<CefBrowser>,
            CefRefPtr<CefFrame>,
            CefString const& targetUrl,
            WindowOpenDisposition,
            bool
        ) override
        {
            Log::warn("Blocked opening '{}' in a new window.", targetUrl.ToString());
            return true;
        }

      private:
        IMPLEMENT_REFCOUNTING(RequestHandler);
    };

    /**
     * @brief File names the page sent with the claim.
     */
    std::vector<std::string> claimedNames(nlohmann::json const& request)
    {
        std::vector<std::string> names;
        if (auto const namesEntry = request.find("names"); namesEntry != request.end() && namesEntry->is_array())
        {
            for (auto const& name : *namesEntry)
            {
                if (name.is_string())
                    names.push_back(name.get<std::string>());
            }
        }
        return names;
    }

    /**
     * @brief Answers NativeFileDrop::claim with the remembered files when the page dropped them.
     */
    Nui::RpcHub::AutoUnregister registerDropClaim(Nui::RpcHub& hub, std::shared_ptr<DraggedFiles> files)
    {
        return hub.autoRegisterFunction(
            "NativeFileDrop::claim",
            [&hub, files = std::move(files)](nlohmann::json const& arguments)
            {
                auto const& request = arguments.is_array() && !arguments.empty() ? arguments[0] : arguments;

                std::vector<SharedData::DroppedItem> entries;
                for (auto const& path : files->take(claimedNames(request)))
                {
                    if (auto item = droppedItemFromPath(path); item)
                        entries.push_back(*std::move(item));
                }
                if (entries.empty())
                {
                    Log::warn("NativeFileDrop::claim: no dropped files with these names are pending.");
                    return;
                }
                hub.callRemote("SessionArea::onFilesDropped", droppedFilesMessage(request, entries));
            }
        );
    }
}

struct CefHooks::Implementation
{
    explicit Implementation(Nui::RpcHub& hub)
        : draggedFiles{std::make_shared<DraggedFiles>()}
        , claimRegistration{registerDropClaim(hub, draggedFiles)}
    {}

    std::shared_ptr<DraggedFiles> draggedFiles;
    Nui::RpcHub::AutoUnregister claimRegistration;
};

CefHooks::CefHooks(Nui::Window& window, Nui::RpcHub& hub)
    : impl_{std::make_unique<Implementation>(hub)}
{
    Nui::Cef::setClientHooks(
        window,
        Nui::Cef::ClientHooks{
            .contextMenuHandler = new ContextMenuHandler(),
            .dragHandler = new DragHandler(impl_->draggedFiles),
            .requestHandler = new RequestHandler(),
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(CefHooks);
