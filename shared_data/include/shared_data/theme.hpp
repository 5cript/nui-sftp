#pragma once

#include <utility/describe.hpp>

namespace SharedData
{
    enum class DarkLightMode
    {
        System,
        Dark,
        Light
    };
    BOOST_DESCRIBE_ENUM(DarkLightMode, System, Dark, Light);
}