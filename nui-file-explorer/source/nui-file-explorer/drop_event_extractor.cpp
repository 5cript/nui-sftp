#include <nui-file-explorer/drop_event_extractor.hpp>
#include <nui-file-explorer/preprocessor.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/json.hpp>

#include <climits>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    namespace
    {
        bool looksLikeAPath(std::string const& text)
        {
            if (text.empty())
                return false;

            if (text.front() == '/')
                return true;

#ifdef PATH_MAX
            if (text.length() > PATH_MAX)
                return false;
#endif

            return text.find('/') != std::string::npos || text.find('\\') != std::string::npos;
        }

        std::vector<std::string> splitByWhitespace(std::string const& text)
        {
            std::vector<std::string> parts{};
            std::string currentPart;
            for (char c : text)
            {
                if (std::isspace(static_cast<unsigned char>(c)))
                {
                    if (!currentPart.empty())
                    {
                        parts.push_back(currentPart);
                        currentPart.clear();
                    }
                }
                else
                {
                    currentPart.push_back(c);
                }
            }
            if (!currentPart.empty())
                parts.push_back(currentPart);
            return parts;
        }

        std::string extractFromHtmlLink(std::string_view html)
        {
            //<a lots of attributes>file:///home/user/...</a>
            if (!html.ends_with("</a>"))
                return {};
            html.remove_suffix(4);
            const auto pos = html.rfind('>');
            if (pos == std::string_view::npos)
                return {};
            std::string_view fileUrl = html.substr(pos + 1);
            if (fileUrl.starts_with("file://"))
                return std::string{fileUrl.substr(7)};
            return {};
        }
    }

    std::optional<DropEventResult>
    extractDropEvent(Nui::WebApi::DragEvent event, ISideModel& sideModel, std::optional<Item> const& droppedOnItem)
    {
        auto dataTransferOpt = event.dataTransfer();
        if (!dataTransferOpt.has_value())
            return std::nullopt;

        const auto types = dataTransferOpt->types();

        DropEventResult result;
        if (droppedOnItem && droppedOnItem->type == Item::Type::Directory)
            result.internalDropSubdir = droppedOnItem->path.filename().string();

        const auto files = dataTransferOpt->files();
        if (files.length() > 0 && STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webview2"s)
        {
            nlohmann::json msg = {
                {"type", "filedrop"},
                {"isLeft", sideModel.isLeft()},
                {"dropMetadata", sideModel.dropMetadata()},
            };
            if (result.internalDropSubdir)
                msg["subdir"] = *result.internalDropSubdir;

            Nui::val::global("chrome")["webview"].call<void>(
                "postMessageWithAdditionalObjects", msg.dump(), files.val()
            );
            result.delegatedToWebView2 = true;
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
            auto split = splitByWhitespace(text);
            for (auto const& part : split)
            {
                if (looksLikeAPath(part))
                {
                    result.externDroppedItems = result.externDroppedItems.value_or(std::vector<Item>{});
                    result.externDroppedItems->push_back(Item{SharedData::DirectoryEntry{.path = part}});
                }
            }
            return result;
        }

        const auto hasTextHtml = std::find(types.begin(), types.end(), "text/html") != types.end();
        if (hasTextHtml)
        {
            Nui::WebApi::Console::log("Data transfer has text/html.");
            auto html = dataTransferOpt->getData("text/html");
            auto extractedLink = extractFromHtmlLink(html);
            if (!extractedLink.empty())
            {
                Nui::WebApi::Console::log("Extracted link from html: " + extractedLink);
                if (looksLikeAPath(extractedLink))
                {
                    result.externDroppedItems = result.externDroppedItems.value_or(std::vector<Item>{});
                    result.externDroppedItems->push_back(Item{SharedData::DirectoryEntry{.path = extractedLink}});
                }
            }
            result.issueWebkitWarning =
                (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s || STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkit"s);
            return result;
        }

        const auto hasUriList = std::find(types.begin(), types.end(), "text/uri-list") != types.end();
        if (hasUriList)
        {
            // Always empty because of: https://github.com/WebKit/WebKit/commit/89838b9164a1dd3baa7053539cf93414977fb081
            // Improper fix of: https://www.cve.org/CVERecord?id=CVE-2025-13947

            auto uriList = dataTransferOpt->getData("text/uri-list");
            if (!uriList.empty() &&
                (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s || STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkit"s))
            {
                Nui::WebApi::Console::log("URI list dropped: " + uriList);
            }
            result.issueWebkitWarning =
                (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s || STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkit"s);
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
                sideModel.onError(std::string{"Failed to parse drag and drop data: "} + e.what());
                return std::nullopt;
            }
        }
        return result;
    }
}