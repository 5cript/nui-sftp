#include <backend/opener.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <fcntl.h>

#include <map>

namespace
{
    constexpr char const* portalService = "org.freedesktop.portal.Desktop";
    constexpr char const* portalObjectPath = "/org/freedesktop/portal/desktop";
    constexpr char const* openUriInterface = "org.freedesktop.portal.OpenURI";
}

struct Opener::Implementation
{
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IProxy> openUriProxy;

    Implementation()
        : connection{sdbus::createSessionBusConnection()}
        , openUriProxy{
              sdbus::createProxy(*connection, sdbus::ServiceName{portalService}, sdbus::ObjectPath{portalObjectPath})
          }
    {}
};

Opener::Opener()
    : impl_{std::make_unique<Implementation>()}
{}

Opener::~Opener() = default;

Opener::Opener(Opener&&) noexcept = default;

Opener& Opener::operator=(Opener&&) noexcept = default;

std::expected<void, std::string> Opener::openFile(std::filesystem::path const& path, bool openWith)
{
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
        return std::unexpected{std::string{"Failed to open file: "} + std::strerror(errno)};

    std::map<std::string, sdbus::Variant> options;
    if (openWith)
        options.emplace("ask", sdbus::Variant{true});
    else
        options.emplace("ask", sdbus::Variant{false});

    try
    {
        sdbus::ObjectPath handle;
        impl_->openUriProxy->callMethod("OpenFile")
            .onInterface(openUriInterface)
            .withArguments(std::string{""}, sdbus::UnixFd{fd}, options)
            .storeResultsTo(handle);
    }
    catch (sdbus::Error const& err)
    {
        return std::unexpected{err.getMessage()};
    }

    return {};
}
