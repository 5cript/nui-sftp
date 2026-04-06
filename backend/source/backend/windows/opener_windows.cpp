#include <backend/opener.hpp>

#include <nui/utility/utf.hpp>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

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

    std::expected<void, std::string> checkAttachmentPolicy(std::filesystem::path const& path)
    {
        winrt::com_ptr<IAttachmentExecute> attach;
        HRESULT hr = CoCreateInstance(CLSID_AttachmentServices, nullptr, CLSCTX_ALL, IID_PPV_ARGS(attach.put()));
        if (FAILED(hr))
            return {}; // COM unavailable — fail open, not fail closed

        attach->SetFileName(path.wstring().c_str());

        hr = attach->CheckPolicy();
        if (hr == S_FALSE)
            return std::unexpected{"File type is blocked by attachment policy"};
        if (FAILED(hr))
            return std::unexpected{
                "Attachment policy check failed: HRESULT 0x" + [hr]()
                {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
                    return std::string{buf};
                }()
            };

        return {};
    }
}

struct Opener::Implementation
{};

Opener::Opener()
    : impl_{std::make_unique<Implementation>()}
{}

Opener::~Opener() = default;

Opener::Opener(Opener&&) noexcept = default;

Opener& Opener::operator=(Opener&&) noexcept = default;

std::expected<void, std::string> Opener::openFile(std::filesystem::path const& path, bool openWith)
{
    // Refuse to open binary executables (content-based, not extension-based)
    DWORD binaryType{};
    if (GetBinaryType(path.wstring().c_str(), &binaryType))
        return std::unexpected{"File is a binary executable"};

    // Windows attachment policy (respects system/zone policy)
    if (auto result = checkAttachmentPolicy(path); !result)
        return result;

    if (openWith)
    {
        OPENASINFO info{};
        info.pcszFile = path.wstring().c_str();
        info.oaifInFlags = OAIF_EXEC | OAIF_ALLOW_REGISTRATION;

        HRESULT const hr = SHOpenWithDialog(nullptr, &info);
        if (FAILED(hr))
            return std::unexpected{
                "SHOpenWithDialog failed: HRESULT 0x" + [hr]()
                {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
                    return std::string{buf};
                }()
            };
    }
    else
    {
        INT_PTR const result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL)
        );
        if (result <= 32)
            return std::unexpected{shellExecuteErrorToString(result)};
    }

    return {};
}
