#include <backend/opener.hpp>
#include <utility/localized_message.hpp>

#include <nui/utility/utf.hpp>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <string>
#include <system_error>

namespace
{
    std::string shellExecuteErrorToString(INT_PTR code)
    {
        switch (code)
        {
            case 0:
                return "backend.opener.windows.outOfResources";
            case 2:
                return "backend.opener.windows.fileNotFound";
            case 3:
                return "backend.opener.windows.pathNotFound";
            case 5:
                return "backend.opener.windows.accessDenied";
            case 8:
                return "backend.opener.windows.insufficientMemory";
            case 26:
                return "backend.opener.windows.sharingViolation";
            case 27:
                return "backend.opener.windows.associationIncomplete";
            case 28:
                return "backend.opener.windows.ddeTimeout";
            case 29:
                return "backend.opener.windows.ddeFailed";
            case 30:
                return "backend.opener.windows.ddeBusy";
            case 31:
                return "backend.opener.windows.noAssociation";
            case 32:
                return "backend.opener.windows.dllNotFound";
            default:
                return Utility::localizedMessage("backend.opener.windows.unknownShellError", code);
        }
    }

    std::expected<void, std::string> checkExtensionPolicy(std::filesystem::path const& path)
    {
        auto const ext = path.extension().wstring();
        if (AssocIsDangerous(ext.c_str()))
            return std::unexpected{"backend.opener.windows.blockedByPolicy"};
        return {};
    }
}

struct Opener::Implementation
{};

Opener::Opener(void* /*nativeWindow*/)
    : impl_{std::make_unique<Implementation>()}
{}

Opener::~Opener() = default;

Opener::Opener(Opener&&) noexcept = default;

Opener& Opener::operator=(Opener&&) noexcept = default;

std::expected<void, std::string> Opener::openFile(std::filesystem::path const& path, bool openWith)
{
    const auto pathStrU16 = path.native();
    const auto pathWstr = std::wstring{pathStrU16.begin(), pathStrU16.end()};

    // Refuse to open binary executables (content-based, not extension-based)
    DWORD binaryType{};
    if (GetBinaryTypeW(pathWstr.c_str(), &binaryType))
        return std::unexpected{"backend.opener.windows.binaryExecutable"};

    // Windows attachment policy (respects system/zone policy)
    // If I keep this, it prevents opening of folders:

    // if (auto result = checkExtensionPolicy(path); !result)
    //     return result;

    if (openWith)
    {
        if (!std::filesystem::exists(path) || !path.is_absolute())
            return std::unexpected{"backend.opener.windows.fileNotFound"};

        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.nShow = SW_SHOWNORMAL;
        sei.lpVerb = L"openas";
        sei.lpFile = pathWstr.c_str();
        sei.fMask = SEE_MASK_INVOKEIDLIST; // add this line in your code
        const auto winBoolResult = ShellExecuteExW(&sei);
        if (!winBoolResult)
        {
            const auto error = GetLastError();
            return std::unexpected{Utility::localizedMessage(
                "backend.opener.windows.shellExecuteExFailed",
                std::system_category().message(static_cast<int>(error)),
                error
            )};
        }
    }
    else
    {
        INT_PTR const result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", pathWstr.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
        );
        if (result <= 32)
            return std::unexpected{shellExecuteErrorToString(result)};
    }

    return {};
}

SharedData::OpenerCapabilities Opener::capabilities() const
{
    // ShellExecuteW / SHOpenFolderAndSelectItems are always available on any supported Windows;
    // there's no probe short of trying the call with a real file, so report success.
    return SharedData::OpenerCapabilities{};
}

std::expected<void, std::string> Opener::openInFileManager(std::filesystem::path const& path)
{
    const auto pathStrU16 = path.native();
    const auto pathWstr = std::wstring{pathStrU16.begin(), pathStrU16.end()};

    std::error_code directoryCheckEc;
    const bool isDirectory = std::filesystem::is_directory(path, directoryCheckEc);

    if (isDirectory)
    {
        // Directory: open Explorer at this directory via the "explore" verb.
        INT_PTR const result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"explore", pathWstr.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
        );
        if (result <= 32)
            return std::unexpected{shellExecuteErrorToString(result)};
        return {};
    }

    // File: reveal it in Explorer (opens the parent and highlights the item).
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT parseHr = SHParseDisplayName(pathWstr.c_str(), nullptr, &pidl, 0, nullptr);
    if (FAILED(parseHr) || !pidl)
        return std::unexpected{"SHParseDisplayName failed"};

    const HRESULT openHr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    if (FAILED(openHr))
        return std::unexpected{"SHOpenFolderAndSelectItems failed"};

    return {};
}
