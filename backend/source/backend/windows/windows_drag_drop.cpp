
#include <backend/windows/windows_drag_drop.hpp>
#include <backend/windows/cotask_mem_string.hpp>
#include <shared_data/directory_entry.hpp>

#include <utility/path_utf.hpp>

#include <nui/utility/scope_exit.hpp>

#include <WebView2.h>
#include <wrl/event.h>
#include <winrt/base.h>

#include <iostream>
#include <filesystem>

struct EnableWindowsDragDrop::Implementation
{
    EventRegistrationToken webMessageReceivedToken;
};

template <>
_GUID const& __mingw_uuidof<ICoreWebView2WebMessageReceivedEventHandler>()
{
    return IID_ICoreWebView2WebMessageReceivedEventHandler;
}

namespace
{
    SharedData::DirectoryEntry leanDirectoryEntryFromPath(std::filesystem::path const& path)
    {
        using FileType = SharedData::DirectoryEntry::FileType;

        return SharedData::DirectoryEntry{
            .path = path,
            .type = [&path]()
            {
                const auto status = std::filesystem::status(path);
                if (std::filesystem::is_directory(status))
                    return FileType::Directory;
                else if (std::filesystem::is_regular_file(status))
                    return FileType::Regular;
                else if (std::filesystem::is_symlink(status))
                    return FileType::Symlink;
                else if (std::filesystem::is_socket(status))
                    return FileType::Socket;
                else if (std::filesystem::is_character_file(status))
                    return FileType::CharDevice;
                else if (std::filesystem::is_block_file(status))
                    return FileType::BlockDevice;
                else if (std::filesystem::is_other(status))
                    return FileType::Special;
                else
                    return FileType::Unknown;
            }()
        };
    }
}

EnableWindowsDragDrop::EnableWindowsDragDrop(Nui::Window& wnd, Nui::RpcHub& hub)
    : impl_{std::make_unique<Implementation>()}
{
    auto* webview2 = static_cast<ICoreWebView2*>(wnd.getNativeWebView());
    webview2->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [&hub](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
            {
                // Args2
                winrt::com_ptr<ICoreWebView2WebMessageReceivedEventArgs2> args2{
                    [args]()
                    {
                        ICoreWebView2WebMessageReceivedEventArgs2* args2Raw = nullptr;
                        args->QueryInterface(
                            IID_ICoreWebView2WebMessageReceivedEventArgs2, reinterpret_cast<void**>(&args2Raw)
                        );
                        return args2Raw;
                    }(),
                    winrt::take_ownership_from_abi_t{}
                };
                if (args2 == nullptr)
                    return S_OK;

                CoTaskMemString source{nullptr};
                args2->get_Source(source.recepticle());
                if (source.toStandardString() != L"nui://app.example/index.html")
                {
                    std::wcout << "Ignoring WebMessageReceived from source: " << source.toStandardString() << L"\n";
                    return S_OK;
                }

                CoTaskMemString webMessage{nullptr};
                args2->get_WebMessageAsJson(webMessage.recepticle());
                nlohmann::json messageJson;
                try
                {
                    // I know this matroschka is stupid, but I dont know how to improve easily:
                    messageJson =
                        nlohmann::json::parse(nlohmann::json::parse(webMessage.toUtf8String()).get<std::string>());
                    if (!messageJson.contains("type") || messageJson["type"] != "filedrop")
                    {
                        return S_OK;
                    }
                }
                catch (std::exception const& exc)
                {
                    std::wcout << L"Failed to parse message as JSON." << std::endl;
                    std::cout << exc.what();
                    return S_OK;
                }

                // Object collection
                winrt::com_ptr<ICoreWebView2ObjectCollectionView> objectsCollection{
                    [&args2]()
                    {
                        ICoreWebView2ObjectCollectionView* collection = nullptr;
                        args2->get_AdditionalObjects(&collection);
                        return collection;
                    }(),
                    winrt::take_ownership_from_abi_t{}
                };
                if (objectsCollection == nullptr)
                    return S_OK;

                unsigned int length{0};
                objectsCollection->get_Count(&length);

                // Array of file paths to be sent back to the webview as JSON
                std::vector<SharedData::DirectoryEntry> entries;
                for (unsigned int i = 0; i < length; i++)
                {
                    winrt::com_ptr<IUnknown> object{
                        [&objectsCollection, i]()
                        {
                            IUnknown* rawUnkown = nullptr;
                            objectsCollection->GetValueAtIndex(i, &rawUnkown);
                            return rawUnkown;
                        }(),
                        winrt::take_ownership_from_abi_t{}
                    };
                    if (object == nullptr)
                        continue;

                    winrt::com_ptr<ICoreWebView2File> file{
                        [&object]()
                        {
                            ICoreWebView2File* fileRaw = nullptr;
                            object.as(IID_ICoreWebView2File, reinterpret_cast<void**>(&fileRaw));
                            return fileRaw;
                        }(),
                        winrt::take_ownership_from_abi_t{}
                    };
                    if (file)
                    {
                        // Add the file to message to be sent back to webview
                        CoTaskMemString pathStr{nullptr};
                        file->get_Path(pathStr.recepticle());
                        entries.emplace_back(leanDirectoryEntryFromPath(
                            Utility::pathToUtf8Generic(std::filesystem::path{pathStr.toStandardString()})
                        ));
                    }
                }

                try
                {
                    auto reply = nlohmann::json{
                        {"entries", entries},
                        {"dropMetadata", messageJson.value("dropMetadata", "")},
                    };

                    if (messageJson.contains("isLeft"))
                        reply["isLeft"] = messageJson["isLeft"];

                    if (messageJson.contains("subdir"))
                        reply["subdir"] = messageJson["subdir"];

                    hub.callRemote("SessionArea::onFilesDropped", reply);
                }
                catch (std::exception const& exc)
                {
                    std::wcout << L"Failed to send drop message to RPC hub." << std::endl;
                    std::cout << exc.what();
                }

                return S_OK;
            }
        ).Get(),
        &impl_->webMessageReceivedToken
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(EnableWindowsDragDrop);