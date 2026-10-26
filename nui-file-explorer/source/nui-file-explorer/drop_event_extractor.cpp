#include <nui-file-explorer/drop_event_extractor.hpp>
#include <nui-file-explorer/drop_text.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <nui-file-explorer/transfer_target.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/file_list.hpp>
#include <nui/frontend/api/json.hpp>
#include <nui/frontend/rpc_client.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    namespace
    {
        /**
         * @brief Names of the dropped files, the backend checks them against the paths it remembered for the drag.
         */
        nlohmann::json droppedFileNames(Nui::WebApi::FileList const& files)
        {
            auto names = nlohmann::json::array();
            for (int index = 0; index < files.length(); ++index)
            {
                if (const auto file = files.item(index); file)
                    names.push_back(file->name());
            }
            return names;
        }
    }

    DropAction suggestedDropAction(Nui::WebApi::DragEvent const& event, bool fromDesktop)
    {
        const auto held = [&event](char const* key)
        {
            const auto value = event.val()[key];
            return value.isTrue();
        };
        if (fromDesktop)
            return held("shiftKey") || held("metaKey") ? DropAction::Move : DropAction::Copy;
        return held("ctrlKey") || held("altKey") ? DropAction::Copy : DropAction::Move;
    }

    std::optional<DropEventResult>
    extractDropEvent(Nui::WebApi::DragEvent event, ISideModel& sideModel, std::optional<Item> const& droppedOnItem)
    {
        auto dataTransferOpt = event.dataTransfer();
        if (!dataTransferOpt.has_value())
            return std::nullopt;

        const auto types = dataTransferOpt->types();

        DropEventResult result;
        if (droppedOnItem)
        {
            auto target = transferTargetOf(*droppedOnItem);
            if (!target)
            {
                sideModel.onError(target.error());
                return std::nullopt;
            }
            result.internalDropSubdir = *std::move(target);
        }

        const auto files = dataTransferOpt->files();
        if (files.length() > 0 && STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webview2"s)
        {
            nlohmann::json msg = {
                {"type", "filedrop"},
                {"isLeft", sideModel.isLeft()},
                {"dropMetadata", sideModel.dropMetadata()},
                {"copy", suggestedDropAction(event, true) == DropAction::Copy},
            };
            if (result.internalDropSubdir)
                msg["subdir"] = *result.internalDropSubdir;

            Nui::val::global("chrome")["webview"].call<void>(
                "postMessageWithAdditionalObjects", msg.dump(), files.val()
            );
            result.delegatedToBackend = true;
            return result;
        }

        // WKWebView and Chromium hide the paths, the backend read them from the dragging pasteboard or the drag data.
        if (files.length() > 0 &&
            (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "wkwebview"s || STRINGIZE_EXPANDED(BROWSER_ENGINE) == "cef"s))
        {
            nlohmann::json claim = {
                {"isLeft", sideModel.isLeft()},
                {"dropMetadata", sideModel.dropMetadata()},
                {"names", droppedFileNames(files)},
                {"copy", suggestedDropAction(event, true) == DropAction::Copy},
            };
            if (result.internalDropSubdir)
                claim["subdir"] = *result.internalDropSubdir;

            Nui::RpcClient::call("NativeFileDrop::claim", Nui::JSON::parse(claim.dump()));
            result.delegatedToBackend = true;
            return result;
        }

        // Nui::WebApi::Console::log(fmt::format("DataTransfer has {} files.", files.length()));
        // for (int i = 0; i < files.length(); ++i)
        // {
        //     auto fileOpt = files.item(i);
        //     if (fileOpt.has_value())
        //     {
        //         Nui::WebApi::Console::log(
        //             fmt::format(
        //                 "Dropped file: {} (last modified: {}), webkitRelativePath: {}",
        //                 fileOpt->name(),
        //                 fileOpt->lastModified(),
        //                 fileOpt->webkitRelativePath()
        //             )
        //         );
        //     }
        // }

        // Nui::WebApi::Console::log(fmt::format("DataTransfer has {} items.", types.size()));
        // const auto items = dataTransferOpt->items();
        // for (int i = 0; i < items.length(); ++i)
        // {
        //     Nui::WebApi::Console::log(fmt::format("Processing DataTransferItem {}", i));
        //     auto itemOpt = items[i];
        //     if (!itemOpt.has_value())
        //     {
        //         Nui::WebApi::Console::log(fmt::format("DataTransferItem {} is null or undefined.", i));
        //         continue;
        //     }
        //     auto& item = *itemOpt;
        //     Nui::WebApi::Console::log(
        //         fmt::format(
        //             "DataTransferItem {}: kind={}, type={}",
        //             i,
        //             (item.kind() == Nui::WebApi::DataTransferItem::Kind::File            ? "file"
        //                     : item.kind() == Nui::WebApi::DataTransferItem::Kind::String ? "string"
        //                                                                                  : "unknown"),
        //             item.type()
        //         )
        //     );

        //     if (item.kind() == Nui::WebApi::DataTransferItem::Kind::File)
        //     {
        //         auto fileOpt = item.getAsFile();
        //         if (fileOpt.has_value())
        //         {
        //             Nui::WebApi::Console::log(
        //                 fmt::format(
        //                     "  -> as File: {} (last modified: {}), webkitRelativePath: {}",
        //                     fileOpt->name(),
        //                     fileOpt->lastModified(),
        //                     fileOpt->webkitRelativePath()
        //                 )
        //             );
        //         }
        //     }
        //     else if (item.kind() == Nui::WebApi::DataTransferItem::Kind::String)
        //     {
        //         item.getAsString(
        //             [](std::optional<std::string> const& s)
        //             {
        //                 if (s.has_value())
        //                     Nui::WebApi::Console::log(fmt::format("  -> as String (callback): {}", *s));
        //                 else
        //                     Nui::WebApi::Console::log("  -> as String (callback): null or undefined");
        //             }
        //         );
        //         Nui::WebApi::Console::log("  -> as String (immediate): cannot get immediate string for async API?");
        //     }

        //     if (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s || STRINGIZE_EXPANDED(BROWSER_ENGINE) ==
        //     "webkit"s)
        //     {
        //         auto fsEntry = item.webkitGetAsEntry();
        //         if (!fsEntry.has_value())
        //         {
        //             continue;
        //         }
        //         Nui::WebApi::Console::log(fmt::format("  -> webkitGetAsEntry: {}", fsEntry->fullPath()));
        //     }
        // }

        const auto hasTextPlain = std::find(types.begin(), types.end(), "text/plain") != types.end();
        if (hasTextPlain)
        {
            const auto text = dataTransferOpt->getData("text/plain");
            for (auto const line : Detail::droppedLines(text))
            {
                if (const auto path = Detail::droppedPathOf(line, Paths::defaultLocalPlatform()); path)
                {
                    result.externDroppedItems = result.externDroppedItems.value_or(std::vector<Item>{});
                    result.externDroppedItems->push_back(Item::dropped(SharedData::DirectoryEntry{}, *path));
                }
            }
            return result;
        }

        const auto hasTextHtml = std::find(types.begin(), types.end(), "text/html") != types.end();
        if (hasTextHtml)
        {
            Nui::WebApi::Console::log("Data transfer has text/html.");
            const auto html = dataTransferOpt->getData("text/html");
            if (const auto extractedLink = Detail::extractFromHtmlLink(html); extractedLink)
            {
                if (const auto path = Detail::droppedPathOf(*extractedLink, Paths::defaultLocalPlatform()); path)
                {
                    result.externDroppedItems = result.externDroppedItems.value_or(std::vector<Item>{});
                    result.externDroppedItems->push_back(Item::dropped(SharedData::DirectoryEntry{}, *path));
                    result.issueWebkitWarning = STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s;
                    return result;
                }
            }
        }

        const auto hasUriList = std::find(types.begin(), types.end(), "text/uri-list") != types.end();
        if (hasUriList)
        {
            // Always empty because of: https://github.com/WebKit/WebKit/commit/89838b9164a1dd3baa7053539cf93414977fb081
            // Improper fix of: https://www.cve.org/CVERecord?id=CVE-2025-13947

            auto uriList = dataTransferOpt->getData("text/uri-list");
            if (!uriList.empty() && STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s)
            {
                Nui::WebApi::Console::log("URI list dropped: " + uriList);
            }
            result.issueWebkitWarning = STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s;
            return result;
        }

        const auto hasApplicationJson = std::find(types.begin(), types.end(), "application/json") != types.end();
        if (hasApplicationJson)
        {
            try
            {
                const auto jsonData = dataTransferOpt->getData("application/json");
                auto info = Nui::JSON::parse(jsonData);
                if (info.hasOwnProperty("isLeft"))
                {
                    result.isInternalDropFromLeftSide = info["isLeft"].as<bool>();
                }
            }
            catch (std::exception const& e)
            {
                sideModel.onError(Error{.code = ErrorCode::DropDataUnreadable, .detail = e.what()});
                return std::nullopt;
            }
        }
        return result;
    }
}