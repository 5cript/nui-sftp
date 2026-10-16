#pragma once

#include <expected>
#include <string>

struct ProgramOptions
{
    bool enableDevTools = false;
};

/**
 * @brief What the program prints instead of starting: the help, or a parse error followed by the help.
 */
struct ProgramOptionsExit
{
    std::string message;
    /**
     * @brief True for a parse error, which goes to the error stream and exits with 1.
     */
    bool isError = false;
};

/**
 * @brief The options of the command line, or what to print and exit with when the help is requested or an argument
 *        is not understood.
 */
std::expected<ProgramOptions, ProgramOptionsExit> parseProgramOptions(int argc, char const* const* argv);
