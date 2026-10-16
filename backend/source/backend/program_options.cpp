#include <backend/program_options.hpp>
#include <utility/error_message.hpp>

#include <boost/program_options.hpp>
#include <fmt/format.h>

#include <sstream>

std::expected<ProgramOptions, ProgramOptionsExit> parseProgramOptions(int argc, char const* const* argv)
{
    boost::program_options::options_description description("Allowed options");
    description.add_options()("help", "produce help message")(
        "enable-dev-tools", "Enable developer tools (DevTools in Browser)"
    )("console", "Windows: write output to the console the program was started from");

    const auto help = [&description]()
    {
        std::ostringstream stream;
        stream << description;
        return stream.str();
    };

    try
    {
        boost::program_options::variables_map variables;
        boost::program_options::store(boost::program_options::parse_command_line(argc, argv, description), variables);
        boost::program_options::notify(variables);

        if (variables.count("help"))
            return std::unexpected{ProgramOptionsExit{.message = help(), .isError = false}};

        ProgramOptions options;
#ifdef NDEBUG
        options.enableDevTools = variables.count("enable-dev-tools") > 0;
#else
        options.enableDevTools = true;
#endif
        return options;
    }
    catch (std::exception const& exception)
    {
        return std::unexpected{ProgramOptionsExit{
            .message = fmt::format("Error parsing program options: {}\n\n{}", Utility::errorMessage(exception), help()),
            .isError = true,
        }};
    }
}
