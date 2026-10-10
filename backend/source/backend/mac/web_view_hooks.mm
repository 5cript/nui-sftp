#include <backend/mac/web_view_hooks.hpp>
#include <backend/dropped_entry.hpp>
#include <log/log.hpp>
#include <shared_data/directory_entry.hpp>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <objc/runtime.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

namespace
{
    // A claim this long after the drop belongs to a different gesture.
    constexpr auto pendingDropLifetime = std::chrono::seconds{10};

    struct PendingDrop
    {
        std::mutex mutex;
        std::vector<std::filesystem::path> paths;
        std::chrono::steady_clock::time_point droppedAt;
    };

    PendingDrop& pendingDrop()
    {
        static PendingDrop drop;
        return drop;
    }

    void rememberDroppedFiles(id<NSDraggingInfo> sender)
    {
        NSArray<NSURL*>* const urls = [[sender draggingPasteboard]
            readObjectsForClasses:@[[NSURL class]]
                          options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];

        std::vector<std::filesystem::path> paths;
        for (NSURL* url in urls)
        {
            if (url.fileURL && url.path != nil)
                paths.emplace_back(url.path.UTF8String);
        }

        auto& drop = pendingDrop();
        std::scoped_lock lock{drop.mutex};
        drop.paths = std::move(paths);
        drop.droppedAt = std::chrono::steady_clock::now();
    }

    std::vector<std::filesystem::path> takeDroppedFiles()
    {
        auto& drop = pendingDrop();
        std::scoped_lock lock{drop.mutex};
        auto paths = std::move(drop.paths);
        drop.paths.clear();
        if (std::chrono::steady_clock::now() - drop.droppedAt > pendingDropLifetime)
            return {};
        return paths;
    }

    bool isNavigationItem(NSMenuItem* item)
    {
        // WebKit's private menu item tags, the identifiers are not public API either.
        static NSSet<NSString*>* const navigationIdentifiers = [NSSet setWithArray:@[
            @"WKMenuItemIdentifierReload",
            @"WKMenuItemIdentifierGoBack",
            @"WKMenuItemIdentifierGoForward",
            @"WKMenuItemIdentifierStop",
        ]];
        return item.identifier != nil && [navigationIdentifiers containsObject:item.identifier];
    }
}

@interface NuiSftpWebView : WKWebView
@end

@implementation NuiSftpWebView
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    rememberDroppedFiles(sender);
    return [super performDragOperation:sender];
}

- (void)willOpenMenu:(NSMenu*)menu withEvent:(NSEvent*)event
{
    for (NSMenuItem* item in [menu.itemArray copy])
    {
        if (isNavigationItem(item))
            [menu removeItem:item];
    }
    [super willOpenMenu:menu withEvent:event];
}
@end

struct MacWebViewHooks::Implementation
{
    Nui::RpcHub::AutoUnregister claimRegistration;
};

MacWebViewHooks::MacWebViewHooks(Nui::Window& window, Nui::RpcHub& hub)
    : impl_{std::make_unique<Implementation>(Implementation{
          .claimRegistration = hub.autoRegisterFunction(
              "NativeFileDrop::claim",
              [&hub](nlohmann::json const& arguments)
              {
                  const auto& request = arguments.is_array() && !arguments.empty() ? arguments[0] : arguments;

                  std::vector<SharedData::DirectoryEntry> entries;
                  for (auto const& path : takeDroppedFiles())
                      entries.push_back(leanDirectoryEntryFromPath(path));
                  if (entries.empty())
                  {
                      Log::warn("NativeFileDrop::claim: no dropped files are pending.");
                      return;
                  }

                  auto reply = nlohmann::json{
                      {"entries", entries},
                      {"dropMetadata", request.value("dropMetadata", "")},
                  };
                  if (request.contains("isLeft"))
                      reply["isLeft"] = request["isLeft"];
                  if (request.contains("subdir"))
                      reply["subdir"] = request["subdir"];
                  hub.callRemote("SessionArea::onFilesDropped", reply);
              }
          ),
      })}
{
    auto* const webView = (__bridge WKWebView*)window.getNativeWebView();
    if (webView == nil)
    {
        Log::error("MacWebViewHooks: no native web view, file drops will not carry paths.");
        return;
    }
    // The subclass adds no state, so swapping the class of the live instance is safe.
    if ([webView class] == [WKWebView class])
        object_setClass(webView, [NuiSftpWebView class]);
    else
        Log::warn("MacWebViewHooks: unexpected web view class {}.", NSStringFromClass([webView class]).UTF8String);
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(MacWebViewHooks);
