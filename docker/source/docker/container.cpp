#include <docker/container.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <ranges>
#include <utility>

namespace Docker
{
    namespace
    {
        std::string quoteAll(std::vector<std::string> const& arguments)
        {
            return fmt::format("{}", fmt::join(arguments | std::views::transform(quote), " "));
        }

        std::string trimmed(std::string text)
        {
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            return text;
        }
    }

    std::expected<Container, std::string> Container::start(Options const& options)
    {
        auto name = uniqueContainerName(options.namePrefix);
        const auto started = run(fmt::format(
            "docker run --detach --name {} {} {} {}",
            quote(name),
            quoteAll(options.runArguments),
            quote(options.image),
            quoteAll(options.command)
        ));
        if (started.exitCode != 0)
        {
            removeContainer(name);
            return std::unexpected{fmt::format("starting {} failed: {}", options.image, started.output)};
        }
        return Container{std::move(name)};
    }

    Container::Container(std::string name)
        : name_{std::move(name)}
    {}

    Container::~Container()
    {
        if (!name_.empty())
            removeContainer(name_);
    }

    Container::Container(Container&& other) noexcept
        : name_{std::exchange(other.name_, {})}
    {}

    Container& Container::operator=(Container&& other) noexcept
    {
        if (this != &other)
        {
            if (!name_.empty())
                removeContainer(name_);
            name_ = std::exchange(other.name_, {});
        }
        return *this;
    }

    std::expected<std::string, std::string> Container::ipAddress() const
    {
        const auto inspect = run(fmt::format(
            "docker inspect --format '{{{{range .NetworkSettings.Networks}}}}{{{{.IPAddress}}}} {{{{end}}}}' {}",
            quote(name_)
        ));
        if (inspect.exitCode != 0)
            return std::unexpected{fmt::format("inspecting {} failed: {}", name_, inspect.output)};
        auto address = trimmed(inspect.output);
        address = address.substr(0, address.find(' '));
        if (address.empty())
            return std::unexpected{fmt::format("{} has no network address", name_)};
        return address;
    }

    CommandResult Container::execute(std::string_view command, std::string_view user) const
    {
        const auto userArgument = user.empty() ? std::string{} : fmt::format("--user {} ", quote(user));
        return run(fmt::format("docker exec {}{} sh -c {}", userArgument, quote(name_), quote(command)));
    }
}
