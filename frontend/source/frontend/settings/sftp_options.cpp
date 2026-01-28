#include <frontend/settings/sftp_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/subgroup.hpp>

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
        .customPermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "downloadOptions",
                "customPermissionsHelpText"
            ),
            onChange,
            nulloptReset(downloadOptions.customPermissions, onChange),
            {
                .minValue = 0,
                .maxValue = 0x1FF, // 0o777, octal literal is a clang extension
                .numberBase = decltype(SftpOptions::DownloadOptions::customPermissions)::NumberBase::Octal,
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
        .customPermissions{
            language->getObserved(
                "settings",
                "sftpOptions",
                "uploadOptions",
                "customPermissionsHelpText"
            ),
            onChange,
            nulloptReset(uploadOptions.customPermissions, onChange),
            {
                .minValue = 0,
                .maxValue = 0x1FF, // 0o777, octal literal is a clang extension
                .numberBase = decltype(SftpOptions::UploadOptions::customPermissions)::NumberBase::Octal,
            }
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
            .maxValue = 600,
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
                .customPermissions = uShortOptionalToFilesystemPermsOptional(downloadOptions.customPermissions.value()),
            },
            .reserveSpace = downloadOptions.reserveSpace.value(),
            .doCleanup = downloadOptions.doCleanup.value(),
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
            .customPermissions = uShortOptionalToFilesystemPermsOptional(uploadOptions.customPermissions.value()),
        }};
    }

    state.concurrency = concurrency.value();
    state.operationTimeout = std::chrono::seconds{operationTimeoutSeconds.value()};
    state.defaultDirectory = defaultDirectory.value();
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
        downloadOptions.customPermissions.value(
            filesystemPermsOptionalToUShortOptional(state.downloadOptions->customPermissions)
        );
        downloadOptions.reserveSpace.value(state.downloadOptions->reserveSpace);
        downloadOptions.doCleanup.value(state.downloadOptions->doCleanup);
    }
    else
    {
        downloadOptionsEngaged = false;
        // All defaults:
        downloadOptions.tempFileSuffix.value(std::nullopt);
        downloadOptions.mayOverwrite.value(std::nullopt);
        downloadOptions.tryContinue.value(std::nullopt);
        downloadOptions.inheritPermissions.value(std::nullopt);
        downloadOptions.customPermissions.value(std::nullopt);
        downloadOptions.reserveSpace.value(std::nullopt);
        downloadOptions.doCleanup.value(std::nullopt);
    }

    if (!state.uploadOptions.has_value())
    {
        uploadOptionsEngaged = false;
        // All defaults:
        uploadOptions.tempFileSuffix.value(std::nullopt);
        uploadOptions.mayOverwrite.value(std::nullopt);
        uploadOptions.tryContinue.value(std::nullopt);
        uploadOptions.inheritPermissions.value(std::nullopt);
        uploadOptions.customPermissions.value(std::nullopt);
    }
    else
    {
        uploadOptionsEngaged = true;
        uploadOptions.tempFileSuffix.value(state.uploadOptions->tempFileSuffix);
        uploadOptions.mayOverwrite.value(state.uploadOptions->mayOverwrite);
        uploadOptions.tryContinue.value(state.uploadOptions->tryContinue);
        uploadOptions.inheritPermissions.value(state.uploadOptions->inheritPermissions);
        uploadOptions.customPermissions.value(
            filesystemPermsOptionalToUShortOptional(state.uploadOptions->customPermissions)
        );
    }

    concurrency.value(state.concurrency);
    operationTimeoutSeconds.value(state.operationTimeout.count());
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
        downloadOptions.customPermissions.inherit(
            filesystemPermsOptionalToUShortOptional(state.downloadOptions->customPermissions)
        );
        downloadOptions.reserveSpace.inherit(state.downloadOptions->reserveSpace);
        downloadOptions.doCleanup.inherit(state.downloadOptions->doCleanup);
    }
    else
    {
        downloadOptions.tempFileSuffix.inherit(std::nullopt);
        downloadOptions.mayOverwrite.inherit(std::nullopt);
        downloadOptions.tryContinue.inherit(std::nullopt);
        downloadOptions.inheritPermissions.inherit(std::nullopt);
        downloadOptions.customPermissions.inherit(std::nullopt);
        downloadOptions.reserveSpace.inherit(std::nullopt);
        downloadOptions.doCleanup.inherit(std::nullopt);
    }

    if (state.uploadOptions)
    {
        uploadOptions.tempFileSuffix.inherit(state.uploadOptions->tempFileSuffix);
        uploadOptions.mayOverwrite.inherit(state.uploadOptions->mayOverwrite);
        uploadOptions.tryContinue.inherit(state.uploadOptions->tryContinue);
        uploadOptions.inheritPermissions.inherit(state.uploadOptions->inheritPermissions);
        uploadOptions.customPermissions.inherit(
            filesystemPermsOptionalToUShortOptional(state.uploadOptions->customPermissions)
        );
    }
    else
    {
        uploadOptions.tempFileSuffix.inherit(std::nullopt);
        uploadOptions.mayOverwrite.inherit(std::nullopt);
        uploadOptions.tryContinue.inherit(std::nullopt);
        uploadOptions.inheritPermissions.inherit(std::nullopt);
        uploadOptions.customPermissions.inherit(std::nullopt);
    }

    concurrency.inherit(state.concurrency);
    defaultDirectory.inherit(state.defaultDirectory);
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
                downloadOptions.customPermissions(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "customPermissions")
                ),
                downloadOptions.reserveSpace(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "reserveSpace")
                ),
                downloadOptions.doCleanup(
                    language->getObserved("settings", "sftpOptions", "downloadOptions", "doCleanup")
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
                uploadOptions.customPermissions(
                    language->getObserved("settings", "sftpOptions", "uploadOptions", "customPermissions")
                )
            )
        ),
        concurrency(language->getObserved("settings", "sftpOptions", "concurrency")),
        operationTimeoutSeconds(language->getObserved("settings", "sftpOptions", "operationTimeoutSeconds"))
    );
}
