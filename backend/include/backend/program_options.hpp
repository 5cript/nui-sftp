#pragma once

#include <optional>

struct ProgramOptions
{
    bool enableDevTools = false;
};

std::optional<ProgramOptions> parseProgramOptions(int argc, char const* const* argv);