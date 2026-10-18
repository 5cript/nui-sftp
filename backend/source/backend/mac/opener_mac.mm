#include <backend/opener.hpp>
#include <backend/opener_risk.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>
#include <utility/localized_message.hpp>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <system_error>

namespace
{
    /**
     * @brief @p path with every symlink resolved, so a link is judged and opened as what it points to. @p path itself
     *        when it cannot be resolved.
     */
    std::filesystem::path resolvedPath(std::filesystem::path const& path)
    {
        std::error_code resolveError{};
        const auto resolved = std::filesystem::canonical(path, resolveError);
        return resolveError ? path : resolved;
    }

    NSURL* fileUrl(std::filesystem::path const& path, bool isDirectory)
    {
        NSString* const pathString = [NSString stringWithUTF8String:path.c_str()];
        return [NSURL fileURLWithPath:pathString isDirectory:isDirectory ? YES : NO];
    }

    /**
     * @brief LaunchServices would run a Mach-O or script through Terminal and launch application bundles directly.
     */
    bool isExecutable(std::filesystem::path const& path, bool isDirectory)
    {
        if (isDirectory)
        {
            NSURL* const url = fileUrl(path, true);
            NSNumber* isPackage = nil;
            [url getResourceValue:&isPackage forKey:NSURLIsApplicationKey error:nil];
            return isPackage != nil && [isPackage boolValue];
        }

        struct stat status{};
        if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
            return false;
        return (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    }

    void chooseApplicationAndOpen(NSURL* url)
    {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSOpenPanel* const panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.treatsFilePackagesAsDirectories = NO;
            panel.directoryURL = [NSURL fileURLWithPath:@"/Applications" isDirectory:YES];
            panel.allowedContentTypes = @[UTTypeApplicationBundle];
            panel.prompt = @"Open";
            [panel beginWithCompletionHandler:^(NSModalResponse response) {
                if (response != NSModalResponseOK || panel.URL == nil)
                    return;
                [[NSWorkspace sharedWorkspace] openURLs:@[url]
                                   withApplicationAtURL:panel.URL
                                          configuration:[NSWorkspaceOpenConfiguration configuration]
                                      completionHandler:^(NSRunningApplication*, NSError* error) {
                                          if (error != nil)
                                              Log::error(
                                                  "Opener: open with '{}' failed: {}",
                                                  panel.URL.path.UTF8String,
                                                  error.localizedDescription.UTF8String
                                              );
                                      }];
            }];
        });
    }
}

std::optional<std::string> Opener::riskOf(std::filesystem::path const& path) const
{
    const auto resolved = resolvedPath(path);
    std::error_code statusError{};
    const bool isDirectory = std::filesystem::is_directory(resolved, statusError);
    if (isExecutable(resolved, isDirectory))
        return std::string{isDirectory ? "backend.opener.risk.application" : "backend.opener.risk.program"};
    if (OpenerDetail::hasExtensionIn(resolved, OpenerDetail::macRunnableExtensions))
        return std::string{"backend.opener.risk.dangerousType"};
    return std::nullopt;
}

struct Opener::Implementation
{};

Opener::Opener(std::string /*portalParentWindow*/)
    : impl_{std::make_unique<Implementation>()}
{}

Opener::~Opener() = default;

Opener::Opener(Opener&&) noexcept = default;

Opener& Opener::operator=(Opener&&) noexcept = default;

std::expected<void, std::string> Opener::openFile(std::filesystem::path const& path, bool openWith)
{
    Log::info("Opener: openFile path='{}' openWith={}", Paths::nativeToUtf8(path), openWith);

    std::error_code existsError;
    if (!path.is_absolute() || !std::filesystem::exists(path, existsError))
        return std::unexpected{std::string{"backend.opener.mac.fileNotFound"}};

    // Programs, scripts and bundles are judged by riskOf and the user's policy before this is called, on the same
    // resolved path.
    const auto resolved = resolvedPath(path);
    const bool isDirectory = std::filesystem::is_directory(resolved, existsError);

    @autoreleasepool
    {
        NSURL* const url = fileUrl(resolved, isDirectory);
        if (openWith)
        {
            chooseApplicationAndOpen(url);
            return {};
        }

        if (![[NSWorkspace sharedWorkspace] openURL:url])
            return std::unexpected{Utility::localizedMessage("backend.opener.mac.openFailed", Paths::nativeToUtf8(path))};
    }
    return {};
}

SharedData::OpenerCapabilities Opener::capabilities() const
{
    // NSWorkspace is always present, failures only show up when opening a concrete file.
    return SharedData::OpenerCapabilities{};
}

std::expected<void, std::string> Opener::openInFileManager(std::filesystem::path const& path)
{
    Log::info("Opener: openInFileManager path='{}'", Paths::nativeToUtf8(path));

    std::error_code directoryCheckError;
    const bool isDirectory = std::filesystem::is_directory(path, directoryCheckError);

    @autoreleasepool
    {
        NSWorkspace* const workspace = [NSWorkspace sharedWorkspace];
        if (isDirectory)
        {
            NSString* const pathString = [NSString stringWithUTF8String:path.c_str()];
            if (![workspace selectFile:nil inFileViewerRootedAtPath:pathString])
                return std::unexpected{Utility::localizedMessage("backend.opener.mac.openFailed", Paths::nativeToUtf8(path))};
            return {};
        }
        [workspace activateFileViewerSelectingURLs:@[fileUrl(path, false)]];
    }
    return {};
}
