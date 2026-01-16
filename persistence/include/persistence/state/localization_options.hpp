#pragma once

#include <persistence/state_core.hpp>

#include <string>

namespace Persistence
{
    struct LocalizationOptions
    {
        std::string languageCode{"en_US"};
        std::string dateTimeFormatString{"YYYY-MM-DD HH:mm:ss"};
    };
    BOOST_DESCRIBE_STRUCT(LocalizationOptions, (), (languageCode, dateTimeFormatString))
}