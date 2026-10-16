#include <frontend/file_explorer/open_local_file.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <nui/frontend/rpc_client.hpp>
#include <nui/frontend/val.hpp>

#include <fmt/format.h>

#include <string>

namespace FileExplorer
{
    namespace
    {
        void requestOpen(ConfirmDialog& dialog, Paths::LocalPath const& path, bool openWith, bool confirmed)
        {
            Nui::RpcClient::callWithBackChannel(
                "RpcFilesystem::open",
                [&dialog, path, openWith](Nui::val reply)
                {
                    if (reply.hasOwnProperty("needsConfirmation") && reply["needsConfirmation"].as<bool>())
                    {
                        const auto risk = language->translate(reply["risk"].as<std::string>());
                        dialog.open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                            .headerText = language->get("fileExplorer", "openRiskyHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("fileExplorer", "openRiskyText")), path.toUtf8(), risk
                            ),
                            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
                            .focusButton = ConfirmDialog::Button::No,
                            .onClose =
                                [&dialog, path, openWith](std::optional<ConfirmDialog::Button> button)
                            {
                                if (button == ConfirmDialog::Button::Yes)
                                    requestOpen(dialog, path, openWith, /*confirmed=*/true);
                            },
                        });
                        return;
                    }
                    if (reply.hasOwnProperty("success") && reply["success"].as<bool>())
                        return;

                    const auto error = reply.hasOwnProperty("error")
                        ? language->translate(reply["error"].as<std::string>())
                        : language->get("localSideModel", "invalidResponseFromBackend");
                    Log::error("Failed to open '{}': {}", path.toUtf8(), error);
                    dialog.open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = language->get("localSideModel", "failedToOpenFile"),
                        .text = error,
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                },
                path.toUtf8(),
                openWith,
                confirmed
            );
        }
    }

    void openLocalFile(ConfirmDialog& dialog, Paths::LocalPath const& path, bool openWith)
    {
        requestOpen(dialog, path, openWith, /*confirmed=*/false);
    }
}
