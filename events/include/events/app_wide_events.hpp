#pragma once

#include <nui/event_system/observed_value.hpp>

#include <string>

struct AppWideEvents
{
    Nui::Observed<std::string> onLanguageChanged{"en_US"};
};