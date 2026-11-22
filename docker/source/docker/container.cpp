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

    std::string Container::release() && noexcept
    {
        return std::exchange(name_, {});
    }

    std::expected<std::string, std::string> Container::ipAddress() const
    {
        return containerAddress(name_);
    }

    CommandResult Container::execute(std::string_view command, std::string_view user) const
    {
        return executeIn(name_, command, user);
    }
}
