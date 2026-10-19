#include <backend/rpc_system.hpp>
#include <log/log.hpp>
#include <paths/utf8.hpp>

#include <boost/asio/ip/host_name.hpp>

#ifdef _WIN32
#    include <windows.h>
#    include <Lmcons.h>
#    include <nui/utility/utf.hpp>
#else
#    include <unistd.h>
#    include <pwd.h>
#    include <sys/param.h>
#    include <climits>
#endif

namespace
{
#ifdef __APPLE__
    constexpr std::size_t loginNameMaximum = MAXLOGNAME;
#elif !defined(_WIN32)
    constexpr std::size_t loginNameMaximum = LOGIN_NAME_MAX;
#endif
}

RpcSystem::RpcSystem(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub)
    : RpcHelper::StrandRpc{executor, wnd, hub}
{
    registerGetUsername();
    registerGetHostname();
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

void RpcSystem::registerGetHostname()
{
    on("RpcSystem::getHostname")
        .perform(
            [this](RpcHelper::RpcOnce&& reply)
            {
                reply({{"hostname", hostname()}, {"success", true}});
            }
        );
}

std::string RpcSystem::hostname() const
{
    if (!hostnameMemo_.empty())
        return hostnameMemo_;

    boost::system::error_code error;
    auto name = boost::asio::ip::host_name(error);
    if (error)
    {
        Log::error("RpcSystem::hostname: {}", error.message());
        return "unknown";
    }
    hostnameMemo_ = Paths::sanitizedUtf8(name);
    return hostnameMemo_;
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
        usernameMemo_ = Paths::wideToUtf8(std::wstring_view{username});
        return usernameMemo_;
    }
    else
    {
        Log::error("RpcSystem::username: GetUserNameW failed with error code {}", GetLastError());
        return "unknown";
    }
#elif defined(__linux__) || defined(__APPLE__)
    char username[loginNameMaximum];
    if (getlogin_r(username, sizeof(username)) == 0)
    {
        usernameMemo_ = Paths::sanitizedUtf8(username);
        return usernameMemo_;
    }
    else
    {
        struct passwd* pwd = getpwuid(getuid());
        if (pwd)
            return Paths::sanitizedUtf8(pwd->pw_name);
        Log::error("RpcSystem::username: getlogin_r failed");
        return "unknown";
    }
#endif
}