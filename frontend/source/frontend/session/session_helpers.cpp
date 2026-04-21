#include <frontend/session/session_helpers.hpp>
#include <frontend/icon_from_name.hpp>
#include <log/log.hpp>

#include <ui5-sap-icons/icons/it-host.hpp>
#include <ui5-sap-icons/icons/machine.hpp>

#include <nui/frontend/rpc_client.hpp>
#include <nui/frontend/val.hpp>

namespace SessionInternal
{
    Nui::ElementRenderer resolveIdentityIcon(std::string const& iconName, bool isLocalShell)
    {
        if (!iconName.empty())
            return iconFromName(iconName);
        return isLocalShell ? Ui5Icons::machine() : Ui5Icons::it_host();
    }

    void writeChannelContentToFile(std::filesystem::path const& filePath, std::string const& content)
    {
        Nui::RpcClient::callWithBackChannel(
            "RpcFilesystem::writeFile",
            [filePath](Nui::val response)
            {
                if (!response.hasOwnProperty("success"))
                {
                    Log::error(
                        "Invalid response from RpcFilesystem::writeFile for file '{}'", filePath.generic_string()
                    );
                    return;
                }
                if (!response["success"].as<bool>())
                {
                    const auto error = response["error"].as<std::string>();
                    Log::error("Failed to write file '{}': {}", filePath.generic_string(), error);
                    return;
                }
                Log::info("Successfully wrote file '{}'", filePath.generic_string());
            },
            filePath,
            content
        );
    }
}
