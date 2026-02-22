#include <frontend/settings/sftp_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/subgroup.hpp>
#include <frontend/settings/setting_helper.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

SftpOptions::SftpOptions(std::function<void()> const& onChange)
    : downloadOptions{
        .tempFileSuffix{
            language->getObserved("settings", "sftpOptions", "downloadOptions", "tempFileSuffixHelpText"),
            onChange,
            nulloptReset(downloadOptions.tempFileSuffix, onChange),
        },
        .mayOverwrite{
            language->getObserved("settings", "sftpOptions", "downloadOptions", "mayOverwriteHelpText"),
            onChange,
            nulloptReset(downloadOptions.mayOverwrite, onChange),
        },
        .tryContinue{
            language->getObserved("settings", "sftpOptions", "downloadOptions", "tryContinueHelpText"),
            onChange,
            nulloptReset(downloadOptions.tryContinue, onChange),
        },
        .inheritPermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "inheritPermissionsHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.inheritPermissions, onChange),
        },
        .customFilePermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "customFilePermissionsHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.customFilePermissions, onChange),
            {
                .minValue = 0,
                .maxValue = 0x1FF, // 0o777, octal literal is a clang extension
                .numberBase = decltype(SftpOptions::DownloadOptions::customFilePermissions)::NumberBase::Octal,
            }
        },
        .customDirectoryPermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "customDirectoryPermissionsHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.customDirectoryPermissions, onChange),
            {
                .minValue = 0,
                .maxValue = 0x1FF, // 0o777, octal literal is a clang extension
                .numberBase = decltype(SftpOptions::DownloadOptions::customDirectoryPermissions)::NumberBase::Octal,
            }
        },
        .reserveSpace{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "reserveSpaceHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.reserveSpace, onChange),
        },
        .doCleanup{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "doCleanupHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.doCleanup, onChange),
        },
        .symlinkHandling{
            std::vector<Persistence::SymlinkHandling>{
                Persistence::SymlinkHandling::AsSymlink,
                Persistence::SymlinkHandling::FollowSymlink,
                Persistence::SymlinkHandling::SkipSymlink
            },
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "symlinkHandlingHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.symlinkHandling, onChange),
            [](Persistence::SymlinkHandling linkHandling){
                return Utility::enumToString(linkHandling);
            }
        },
        .failFast{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "failFastHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.failFast, onChange),
        }
    }
    , uploadOptions{
        .tempFileSuffix{
            language->getObserved("settings", "sftpOptions", "uploadOptions", "tempFileSuffixHelpText"),
            onChange,
            nulloptReset(uploadOptions.tempFileSuffix, onChange),
        },
        .mayOverwrite{
            language->getObserved("settings", "sftpOptions", "uploadOptions", "mayOverwriteHelpText"),
            onChange,
            nulloptReset(uploadOptions.mayOverwrite, onChange),
        },
        .tryContinue{
            language->getObserved("settings", "sftpOptions", "uploadOptions", "tryContinueHelpText"),
            onChange,
            nulloptReset(uploadOptions.tryContinue, onChange),
        },
        .inheritPermissions{
            language->getObserved("settings", "sftpOptions", "uploadOptions", "inheritPermissionsHelpText"),
            onChange,
            nulloptReset(uploadOptions.inheritPermissions, onChange),
        },
        .customFilePermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "uploadOptions",
                "customFilePermissionsHelpText"
            ),
            onChange,
            nulloptReset(uploadOptions.customFilePermissions, onChange),
            {
                .minValue = 0,
                .maxValue = 0x1FF, // 0o777, octal literal is a clang extension
                .numberBase = decltype(SftpOptions::UploadOptions::customFilePermissions)::NumberBase::Octal,
            }
        },
        .customDirectoryPermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "uploadOptions",
                "customDirectoryPermissionsHelpText"
            ),
            onChange,
            nulloptReset(uploadOptions.customDirectoryPermissions, onChange),
            {
                .minValue = 0,
                .maxValue = 0x1FF, // 0o777, octal literal is a clang extension
                .numberBase = decltype(SftpOptions::UploadOptions::customDirectoryPermissions)::NumberBase::Octal,
            }
        },
        .symlinkHandling{
            std::vector<Persistence::SymlinkHandling>{
                Persistence::SymlinkHandling::AsSymlink,
                Persistence::SymlinkHandling::FollowSymlink,
                Persistence::SymlinkHandling::SkipSymlink
            },
            language->getObserved(
                "settings",
                "sftpOptions",
                "uploadOptions",
                "symlinkHandlingHelpText"
            ),
            onChange,
            nulloptReset(uploadOptions.symlinkHandling, onChange),
            [](Persistence::SymlinkHandling linkHandling){
                return Utility::enumToString(linkHandling);
            }
        },
        .failFast{
            language->getObserved(
                "settings",
                "sftpOptions",
                "uploadOptions",
                "failFastHelpText"
            ),
            onChange,
            nulloptReset(uploadOptions.failFast, onChange),
        }
    }
    , defaultDirectory{
        language->getObserved("settings", "sftpOptions", "defaultDirectoryHelpText"),
        PathSettingType::Directory,
        onChange,
        nulloptReset(defaultDirectory, onChange),
    }
    , concurrency{
        language->getObserved("settings", "sftpOptions", "concurrencyHelpText"),
        onChange,
        nulloptReset(concurrency, onChange),
        {
            .minValue = 1,
            .maxValue = 20,
        }
    }
    , operationTimeoutSeconds{
        language->getObserved("settings", "sftpOptions", "operationTimeoutSecondsHelpText"),
        onChange,
        [this, onChange](){
            operationTimeoutSeconds.value(5);
            onChange();
        },
        {
            .minValue = 1,
            .maxValue = 60000,
        }
    },
    onChange_{onChange}
{}

void SftpOptions::applyToState(Persistence::SftpOptions& state) const
{
    if (downloadOptionsEngaged.value())
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-designator"
        state.downloadOptions = Persistence::DownloadOptions{
            Persistence::CommonTransferOptions{
                .tempFileSuffix = downloadOptions.tempFileSuffix.value(),
                .mayOverwrite = downloadOptions.mayOverwrite.value(),
                .tryContinue = downloadOptions.tryContinue.value(),
                .inheritPermissions = downloadOptions.inheritPermissions.value(),
                .customFilePermissions = downloadOptions.customFilePermissions.valueIsValid()
                    ? uShortOptionalToFilesystemPermsOptional(downloadOptions.customFilePermissions.value())
                    : std::nullopt,
                .customDirectoryPermissions = downloadOptions.customDirectoryPermissions.valueIsValid()
                    ? uShortOptionalToFilesystemPermsOptional(downloadOptions.customDirectoryPermissions.value())
                    : std::nullopt,
                .symlinkHandling = downloadOptions.symlinkHandling.value(),
                .failFast = downloadOptions.failFast.value(),
            },
            .reserveSpace = downloadOptions.reserveSpace.value(),
            .doCleanup = downloadOptions.doCleanup.value()
        };
#pragma clang diagnostic pop
    }

    if (uploadOptionsEngaged.value())
    {
        state.uploadOptions = Persistence::UploadOptions{Persistence::CommonTransferOptions{
            .tempFileSuffix = uploadOptions.tempFileSuffix.value(),
            .mayOverwrite = uploadOptions.mayOverwrite.value(),
            .tryContinue = uploadOptions.tryContinue.value(),
            .inheritPermissions = uploadOptions.inheritPermissions.value(),
            .customFilePermissions = uploadOptions.customFilePermissions.valueIsValid()
                ? uShortOptionalToFilesystemPermsOptional(uploadOptions.customFilePermissions.value())
                : std::nullopt,
            .customDirectoryPermissions = uploadOptions.customDirectoryPermissions.valueIsValid()
                ? uShortOptionalToFilesystemPermsOptional(uploadOptions.customDirectoryPermissions.value())
                : std::nullopt,
            .symlinkHandling = uploadOptions.symlinkHandling.value(),
            .failFast = uploadOptions.failFast.value(),
        }};
    }

    assignIfValid(state.concurrency, concurrency);
    if (operationTimeoutSeconds.valueIsValid())
    {
        auto timeout = operationTimeoutSeconds.value();
        state.operationTimeout = timeout.has_value() ? std::chrono::seconds{timeout.value()}
                                                     : std::optional<std::chrono::seconds>{std::nullopt};
    }
    assignIfValid(state.defaultDirectory, defaultDirectory);
}

void SftpOptions::loadFromState(Persistence::SftpOptions const& state, bool)
{
    if (state.downloadOptions.has_value())
    {
        downloadOptionsEngaged = true;
        downloadOptions.tempFileSuffix.value(state.downloadOptions->tempFileSuffix);
        downloadOptions.mayOverwrite.value(state.downloadOptions->mayOverwrite);
        downloadOptions.tryContinue.value(state.downloadOptions->tryContinue);
        downloadOptions.inheritPermissions.value(state.downloadOptions->inheritPermissions);
        downloadOptions.customFilePermissions.value(
            filesystemPermsOptionalToUShortOptional(state.downloadOptions->customFilePermissions)
        );
        downloadOptions.customDirectoryPermissions.value(
            filesystemPermsOptionalToUShortOptional(state.downloadOptions->customDirectoryPermissions)
        );
        downloadOptions.reserveSpace.value(state.downloadOptions->reserveSpace);
        downloadOptions.doCleanup.value(state.downloadOptions->doCleanup);
        downloadOptions.symlinkHandling.value(state.downloadOptions->symlinkHandling);
        downloadOptions.failFast.value(state.downloadOptions->failFast);
    }
    else
    {
        downloadOptionsEngaged = false;
        // All defaults:
        downloadOptions.tempFileSuffix.value(std::nullopt);
        downloadOptions.mayOverwrite.value(std::nullopt);
        downloadOptions.tryContinue.value(std::nullopt);
        downloadOptions.inheritPermissions.value(std::nullopt);
        downloadOptions.customFilePermissions.value(std::nullopt);
        downloadOptions.customDirectoryPermissions.value(std::nullopt);
        downloadOptions.reserveSpace.value(std::nullopt);
        downloadOptions.doCleanup.value(std::nullopt);
        downloadOptions.symlinkHandling.value(std::nullopt);
        downloadOptions.failFast.value(std::nullopt);
    }

    if (!state.uploadOptions.has_value())
    {
        uploadOptionsEngaged = false;
        // All defaults:
        uploadOptions.tempFileSuffix.value(std::nullopt);
        uploadOptions.mayOverwrite.value(std::nullopt);
        uploadOptions.tryContinue.value(std::nullopt);
        uploadOptions.inheritPermissions.value(std::nullopt);
        uploadOptions.customFilePermissions.value(std::nullopt);
        uploadOptions.customDirectoryPermissions.value(std::nullopt);
        uploadOptions.symlinkHandling.value(std::nullopt);
        uploadOptions.failFast.value(std::nullopt);
    }
    else
    {
        uploadOptionsEngaged = true;
        uploadOptions.tempFileSuffix.value(state.uploadOptions->tempFileSuffix);
        uploadOptions.mayOverwrite.value(state.uploadOptions->mayOverwrite);
        uploadOptions.tryContinue.value(state.uploadOptions->tryContinue);
        uploadOptions.inheritPermissions.value(state.uploadOptions->inheritPermissions);
        uploadOptions.customFilePermissions.value(
            filesystemPermsOptionalToUShortOptional(state.uploadOptions->customFilePermissions)
        );
        uploadOptions.customDirectoryPermissions.value(
            filesystemPermsOptionalToUShortOptional(state.uploadOptions->customDirectoryPermissions)
        );
        uploadOptions.symlinkHandling.value(state.uploadOptions->symlinkHandling);
        uploadOptions.failFast.value(state.uploadOptions->failFast);
    }

    concurrency.value(state.concurrency);
    operationTimeoutSeconds.value(
        state.operationTimeout ? static_cast<int>(state.operationTimeout->count()) : std::optional<int>{std::nullopt}
    );
    defaultDirectory.value(state.defaultDirectory);
}

void SftpOptions::assumeDefaultsFrom(Persistence::SftpOptions const& state)
{
    if (state.downloadOptions)
    {
        downloadOptions.tempFileSuffix.inherit(state.downloadOptions->tempFileSuffix);
        downloadOptions.mayOverwrite.inherit(state.downloadOptions->mayOverwrite);
        downloadOptions.tryContinue.inherit(state.downloadOptions->tryContinue);
        downloadOptions.inheritPermissions.inherit(state.downloadOptions->inheritPermissions);
        downloadOptions.customFilePermissions.inherit(
            filesystemPermsOptionalToUShortOptional(state.downloadOptions->customFilePermissions)
        );
        downloadOptions.customDirectoryPermissions.inherit(
            filesystemPermsOptionalToUShortOptional(state.downloadOptions->customDirectoryPermissions)
        );
        downloadOptions.reserveSpace.inherit(state.downloadOptions->reserveSpace);
        downloadOptions.doCleanup.inherit(state.downloadOptions->doCleanup);
        downloadOptions.symlinkHandling.inherit(state.downloadOptions->symlinkHandling);
        downloadOptions.failFast.inherit(state.downloadOptions->failFast);
    }
    else
    {
        downloadOptions.tempFileSuffix.inherit(std::nullopt);
        downloadOptions.mayOverwrite.inherit(std::nullopt);
        downloadOptions.tryContinue.inherit(std::nullopt);
        downloadOptions.inheritPermissions.inherit(std::nullopt);
        downloadOptions.customFilePermissions.inherit(std::nullopt);
        downloadOptions.customDirectoryPermissions.inherit(std::nullopt);
        downloadOptions.reserveSpace.inherit(std::nullopt);
        downloadOptions.doCleanup.inherit(std::nullopt);
        downloadOptions.symlinkHandling.inherit(std::nullopt);
        downloadOptions.failFast.inherit(std::nullopt);
    }

    if (state.uploadOptions)
    {
        uploadOptions.tempFileSuffix.inherit(state.uploadOptions->tempFileSuffix);
        uploadOptions.mayOverwrite.inherit(state.uploadOptions->mayOverwrite);
        uploadOptions.tryContinue.inherit(state.uploadOptions->tryContinue);
        uploadOptions.inheritPermissions.inherit(state.uploadOptions->inheritPermissions);
        uploadOptions.customFilePermissions.inherit(
            filesystemPermsOptionalToUShortOptional(state.uploadOptions->customFilePermissions)
        );
        uploadOptions.customDirectoryPermissions.inherit(
            filesystemPermsOptionalToUShortOptional(state.uploadOptions->customDirectoryPermissions)
        );
        uploadOptions.symlinkHandling.inherit(state.uploadOptions->symlinkHandling);
        uploadOptions.failFast.inherit(state.uploadOptions->failFast);
    }
    else
    {
        uploadOptions.tempFileSuffix.inherit(std::nullopt);
        uploadOptions.mayOverwrite.inherit(std::nullopt);
        uploadOptions.tryContinue.inherit(std::nullopt);
        uploadOptions.inheritPermissions.inherit(std::nullopt);
        uploadOptions.customFilePermissions.inherit(std::nullopt);
        uploadOptions.customDirectoryPermissions.inherit(std::nullopt);
        uploadOptions.symlinkHandling.inherit(std::nullopt);
        uploadOptions.failFast.inherit(std::nullopt);
    }

    concurrency.inherit(state.concurrency);
    defaultDirectory.inherit(state.defaultDirectory);
    operationTimeoutSeconds.inherit(
        state.operationTimeout ? static_cast<int>(state.operationTimeout->count()) : std::optional<int>{std::nullopt}
    );
}

Nui::ElementRenderer SftpOptions::render()
{
    using namespace Nui::Elements;

    return fragment(
        subgroup(
            {.engagedStatus = &downloadOptionsEngaged,
                .groupTitle = language->getObserved("settings", "sftpOptions", "downloadOptionsSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                downloadOptions.tempFileSuffix(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "tempFileSuffix")
                ),
                downloadOptions.mayOverwrite(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "mayOverwrite")
                ),
                downloadOptions.tryContinue(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "tryContinue")
                ),
                downloadOptions.inheritPermissions(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "inheritPermissions")
                ),
                downloadOptions.customFilePermissions(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "customFilePermissions")
                ),
                downloadOptions.customDirectoryPermissions(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "customDirectoryPermissions")
                ),
                downloadOptions.reserveSpace(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "reserveSpace")
                ),
                downloadOptions.doCleanup(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "doCleanup")
                ),
                downloadOptions.symlinkHandling(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "symlinkHandling")
                ),
                downloadOptions.failFast(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "failFast")
                )
            )
        ),
        subgroup(
            {.engagedStatus = &uploadOptionsEngaged,
                .groupTitle = language->getObserved("settings", "sftpOptions", "uploadOptionsSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                uploadOptions.tempFileSuffix(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "tempFileSuffix")
                ),
                uploadOptions.mayOverwrite(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "mayOverwrite")
                ),
                uploadOptions.tryContinue(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "tryContinue")
                ),
                uploadOptions.inheritPermissions(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "inheritPermissions")
                ),
                uploadOptions.customFilePermissions(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "customFilePermissions")
                ),
                uploadOptions.customDirectoryPermissions(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "customDirectoryPermissions")
                ),
                uploadOptions.symlinkHandling(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "symlinkHandling")
                ),
                uploadOptions.failFast(language->getObserved("settings", "sftpOptions", "uploadOptions", "failFast"))
            )
        ),
        defaultDirectory(language->getObserved("settings", "sftpOptions", "defaultDirectory")),
        concurrency(language->getObserved("settings", "sftpOptions", "concurrency")),
        operationTimeoutSeconds(language->getObserved("settings", "sftpOptions", "operationTimeoutSeconds"))
    );
}
