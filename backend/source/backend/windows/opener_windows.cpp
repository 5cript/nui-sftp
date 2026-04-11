#include <backend/opener.hpp>

#include <nui/utility/utf.hpp>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <string>

namespace
{
    std::string shellExecuteErrorToString(INT_PTR code)
    {
        switch (code)
        {
            case 0:
                return "Out of memory or resources";
            case 2:
                return "File not found";
            case 3:
                return "Path not found";
            case 5:
                return "Access denied";
            case 8:
                return "Insufficient memory";
            case 26:
                return "Sharing violation";
            case 27:
                return "Filename association is incomplete or invalid";
            case 28:
                return "DDE transaction timed out";
            case 29:
                return "DDE transaction failed";
            case 30:
                return "Other DDE transaction in progress";
            case 31:
                return "No application associated with this file type";
            case 32:
                return "DLL not found";
            default:
                return "Unknown shell error (code " + std::to_string(code) + ")";
        }
    }

    std::expected<void, std::string> checkExtensionPolicy(std::filesystem::path const& path)
    {
        auto const ext = path.extension().wstring();
        if (AssocIsDangerous(ext.c_str()))
            return std::unexpected{"File type is blocked by attachment policy"};
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
        return std::unexpected{"File is a binary executable"};

    // Windows attachment policy (respects system/zone policy)
    // If I keep this, it prevents opening of folders:

    // if (auto result = checkExtensionPolicy(path); !result)
    //     return result;

    if (openWith)
    {
        if (!std::filesystem::exists(path) || !path.is_absolute())
            return std::unexpected{"File not found"};

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
            return std::unexpected{"ShellExecuteExW failed: " + shellExecuteErrorToString(error)};
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
