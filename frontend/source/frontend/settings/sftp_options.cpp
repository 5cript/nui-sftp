#include <frontend/settings/sftp_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>

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
    }
    }
, concurrency{
        language->getObserved("settings", "sftpOptions", "concurrencyHelpText"),
        onChange,
        nulloptReset(concurrency, onChange),
    }
, operationTimeoutSeconds{
        language->getObserved("settings", "sftpOptions", "operationTimeoutSecondsHelpText"),
        onChange,
        [this, onChange](){
            operationTimeoutSeconds.value(5);
            onChange();
        }
    }
{}