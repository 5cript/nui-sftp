#include <nui-file-explorer/drop_event_extractor.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <nui-file-explorer/transfer_target.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/json.hpp>
#include <nui/frontend/rpc_client.hpp>

#include <cctype>
#include <climits>
#include <optional>
#include <string>
#include <string_view>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    namespace
    {
        /**
         * @brief Decodes the %XX escapes of a file URL path, other text stays as it is.
         */
        std::string percentDecoded(std::string_view text)
        {
            const auto hexValue = [](char character) -> int
            {
                if (character >= '0' && character <= '9')
                    return character - '0';
                if (character >= 'a' && character <= 'f')
                    return character - 'a' + 10;
                if (character >= 'A' && character <= 'F')
                    return character - 'A' + 10;
                return -1;
            };
            std::string decoded;
            decoded.reserve(text.size());
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                if (text[index] == '%' && index + 2 < text.size())
                {
                    const auto high = hexValue(text[index + 1]);
                    const auto low = hexValue(text[index + 2]);
                    if (high >= 0 && low >= 0)
                    {
                        decoded.push_back(static_cast<char>(high * 16 + low));
                        index += 2;
                        continue;
                    }
                }
                decoded.push_back(text[index]);
            }
            return decoded;
        }

        /**
         * @brief The absolute local path a dropped text token names: a file URL or an absolute path. Any other text,
         *        like words with a slash in them, is no dropped file.
         */
        std::optional<std::string> droppedPathOf(std::string const& text)
        {
            if (text.empty())
                return std::nullopt;
#ifdef PATH_MAX
            if (text.length() > PATH_MAX)
                return std::nullopt;
#endif
            if (text.starts_with("file://"))
            {
                auto path = percentDecoded(std::string_view{text}.substr(7));
                // file:///C:/x carries the drive after the slash.
                if (path.size() > 3 && path[0] == '/' && path[2] == ':')
                    path.erase(0, 1);
                return path;
            }
            if (text.front() == '/')
                return text;
            const bool hasDrive = text.size() > 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
                text[1] == ':' && (text[2] == '\\' || text[2] == '/');
            if (hasDrive)
                return text;
            return std::nullopt;
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
                return std::string{fileUrl};
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
            };
            if (result.internalDropSubdir)
                msg["subdir"] = *result.internalDropSubdir;

            Nui::val::global("chrome")["webview"].call<void>(
                "postMessageWithAdditionalObjects", msg.dump(), files.val()
            );
            result.delegatedToBackend = true;
            return result;
        }

        // WKWebView hides the paths, the backend read them from the dragging pasteboard.
        if (files.length() > 0 && STRINGIZE_EXPANDED(BROWSER_ENGINE) == "wkwebview"s)
        {
            nlohmann::json claim = {
                {"isLeft", sideModel.isLeft()},
                {"dropMetadata", sideModel.dropMetadata()},
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
            auto split = splitByWhitespace(text);
            for (auto const& part : split)
            {
                if (auto path = droppedPathOf(part); path)
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
            auto html = dataTransferOpt->getData("text/html");
            auto extractedLink = extractFromHtmlLink(html);
            if (!extractedLink.empty())
            {
                if (auto path = droppedPathOf(extractedLink); path)
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