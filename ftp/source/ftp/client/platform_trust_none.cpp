#include <ftp/client/platform_trust.hpp>

namespace FileTransferProtocol
{
    PlatformVerdict platformVerdictOf(
        std::vector<DerCertificate> const&,
        std::string const&,
        std::stop_token const&,
        PlatformAnchors const&
    )
    {
        return PlatformVerdict::Unavailable;
    }
}
