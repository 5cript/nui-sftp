#include <backend/program_options.hpp>

#include <boost/program_options.hpp>

#include <iostream>

std::optional<ProgramOptions> parseProgramOptions(int argc, char const* const* argv)
{
    try
    {
        boost::program_options::options_description desc("Allowed options");
        desc.add_options()("help", "produce help message")(
            "enable-dev-tools", "Enable developer tools (DevTools in Browser)"
        );

        boost::program_options::variables_map vm;
        boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
        boost::program_options::notify(vm);

        if (vm.count("help"))
        {
            std::cout << desc << "\n";
            return std::nullopt;
        }

        ProgramOptions options;
        if (vm.count("enable-dev-tools"))
            options.enableDevTools = true;

        return options;
    }
    catch (std::exception& e)
    {
        std::cerr << "Error parsing program options: " << e.what() << "\n";
        return std::nullopt;
    }
}