#include <backend/rpc_system.hpp>
#include <log/log.hpp>

#ifdef _WIN32
#    include <windows.h>
#    include <Lmcons.h>
#    include <nui/utility/utf.hpp>
#else
#    include <unistd.h>
#    include <pwd.h>
#    include <climits>
#endif

RpcSystem::RpcSystem(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub)
    : RpcHelper::StrandRpc{executor, wnd, hub}
{
    registerGetUsername();
}

void RpcSystem::registerGetUsername()
{
    on("RpcSystem::getUsername")
        .perform(
            [this](RpcHelper::RpcOnce&& reply)
            {
                reply({{"username", username()}, {"success", true}});
            }
        );
}

std::string RpcSystem::username() const
{
    if (!usernameMemo_.empty())
        return usernameMemo_;

#ifdef _WIN32
    wchar_t username[UNLEN + 1];
    DWORD username_len = UNLEN + 1;
    if (GetUserNameW(username, &username_len))
    {
        usernameMemo_ = Nui::utf16ToUtf8<std::wstring, std::string>(std::wstring(username));
        return usernameMemo_;
    }
    else
    {
        Log::error("RpcSystem::username: GetUserNameW failed with error code {}", GetLastError());
        return "unknown";
    }
#elif defined(__linux__) || defined(__APPLE__)
    char username[LOGIN_NAME_MAX];
    if (getlogin_r(username, sizeof(username)) == 0)
    {
        usernameMemo_ = std::string(username);
        return usernameMemo_;
    }
    else
    {
        struct passwd* pwd = getpwuid(getuid());
        if (pwd)
            return pwd->pw_name;
        Log::error("RpcSystem::username: getlogin_r failed");
        return "unknown";
    }
#endif
}