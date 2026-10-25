#include <ssh/async/socket_wait.hpp>

#include <fmt/format.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <fcntl.h>
#    include <poll.h>
#    include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

namespace SecureShell
{
    namespace
    {
        int timeoutMilliseconds(std::optional<std::chrono::steady_clock::time_point> deadline)
        {
            if (!deadline)
                return -1;
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now()).count();
            return static_cast<int>(std::clamp<long long>(remaining, 0, INT_MAX));
        }
    }

#ifdef _WIN32
    struct SocketWait::Implementation
    {
        SOCKET wakeSocket{INVALID_SOCKET};
        bool started{false};

        ~Implementation()
        {
            if (wakeSocket != INVALID_SOCKET)
                closesocket(wakeSocket);
            if (started)
                WSACleanup();
        }

        void drain() noexcept
        {
            char buffer[64];
            while (recv(wakeSocket, buffer, sizeof(buffer), 0) > 0)
            {}
        }
    };

    std::expected<std::unique_ptr<SocketWait>, std::string> SocketWait::create()
    {
        auto impl = std::make_unique<Implementation>();
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return std::unexpected(fmt::format("WSAStartup failed: {}", WSAGetLastError()));
        impl->started = true;

        // A datagram socket connected to itself: wake sends to it, the wait sees it readable.
        impl->wakeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl->wakeSocket == INVALID_SOCKET)
            return std::unexpected(fmt::format("Creating the wake socket failed: {}", WSAGetLastError()));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int length = sizeof(address);
        u_long nonBlocking = 1;
        if (bind(impl->wakeSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            getsockname(impl->wakeSocket, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
            connect(impl->wakeSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ioctlsocket(impl->wakeSocket, FIONBIO, &nonBlocking) != 0)
        {
            return std::unexpected(fmt::format("Setting up the wake socket failed: {}", WSAGetLastError()));
        }
        return std::unique_ptr<SocketWait>{new SocketWait{std::move(impl)}};
    }

    void SocketWait::wake() noexcept
    {
        const char signal = 1;
        send(impl_->wakeSocket, &signal, 1, 0);
    }

    SocketWait::Result
    SocketWait::wait(Watched const& watched, std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        WSAPOLLFD handles[2]{};
        handles[0].fd = watched.socket;
        handles[0].events = static_cast<SHORT>(POLLRDNORM | (watched.writable ? POLLWRNORM : 0));
        handles[1].fd = impl_->wakeSocket;
        handles[1].events = POLLRDNORM;
        const auto result = WSAPoll(handles, 2, timeoutMilliseconds(deadline));
        if (result == SOCKET_ERROR)
            return Result::Failed;
        if (result == 0)
            return Result::TimedOut;
        if (handles[1].revents != 0)
        {
            impl_->drain();
            return Result::Woken;
        }
        return Result::Ready;
    }
#else
    struct SocketWait::Implementation
    {
        int readEnd{-1};
        int writeEnd{-1};

        ~Implementation()
        {
            if (readEnd >= 0)
                ::close(readEnd);
            if (writeEnd >= 0)
                ::close(writeEnd);
        }

        void drain() noexcept
        {
            char buffer[64];
            while (::read(readEnd, buffer, sizeof(buffer)) > 0)
            {}
        }
    };

    std::expected<std::unique_ptr<SocketWait>, std::string> SocketWait::create()
    {
        auto impl = std::make_unique<Implementation>();
        int ends[2]{-1, -1};
#    ifdef __linux__
        // Atomically close-on-exec, a process spawned by another thread meanwhile would inherit the ends.
        if (::pipe2(ends, O_CLOEXEC | O_NONBLOCK) != 0)
            return std::unexpected(fmt::format("Creating the wake pipe failed: {}", std::strerror(errno)));
        impl->readEnd = ends[0];
        impl->writeEnd = ends[1];
#    else
        if (::pipe(ends) != 0)
            return std::unexpected(fmt::format("Creating the wake pipe failed: {}", std::strerror(errno)));
        impl->readEnd = ends[0];
        impl->writeEnd = ends[1];
        for (const auto end : ends)
        {
            if (::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK) != 0 ||
                ::fcntl(end, F_SETFD, FD_CLOEXEC) != 0)
            {
                return std::unexpected(fmt::format("Configuring the wake pipe failed: {}", std::strerror(errno)));
            }
        }
#    endif
        return std::unique_ptr<SocketWait>{new SocketWait{std::move(impl)}};
    }

    void SocketWait::wake() noexcept
    {
        const char signal = 1;
        // A full pipe already wakes the wait.
        while (::write(impl_->writeEnd, &signal, 1) < 0 && errno == EINTR)
        {}
    }

    SocketWait::Result
    SocketWait::wait(Watched const& watched, std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        pollfd handles[2]{};
        handles[0].fd = watched.socket;
        handles[0].events = static_cast<short>(POLLIN | (watched.writable ? POLLOUT : 0));
        handles[1].fd = impl_->readEnd;
        handles[1].events = POLLIN;
        const auto result = ::poll(handles, 2, timeoutMilliseconds(deadline));
        if (result < 0)
            return errno == EINTR ? Result::Woken : Result::Failed;
        if (result == 0)
            return Result::TimedOut;
        if (handles[1].revents != 0)
        {
            impl_->drain();
            return Result::Woken;
        }
        return Result::Ready;
    }
#endif

    SocketWait::SocketWait(std::unique_ptr<Implementation> impl)
        : impl_{std::move(impl)}
    {}
    SocketWait::~SocketWait() = default;
}
