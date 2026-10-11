#pragma once

#include <fmt/format.h>

#include <string>
#include <string_view>

namespace NuiSftpPage
{
    /**
     * @brief Public URL of a product screenshot in the nui-sftp-releases S4 bucket.
     *
     * @param name File name below images/screenshots, e.g. "session.png".
     * @return The absolute URL.
     */
    inline std::string screenshotUrl(std::string_view name)
    {
        return fmt::format(
            "https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-sftp-releases/images/screenshots/{}",
            name
        );
    }
}
