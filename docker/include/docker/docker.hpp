#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace Docker
{
    struct CommandResult
    {
        int exitCode{-1};
        std::string output{};
    };

    /**
     * @brief Runs @p command through /bin/sh and waits for it.
     *
     * @param command A shell command line, see quote() for its arguments.
     * @return The exit code and what the command wrote to stdout and stderr.
     */
    CommandResult run(std::string const& command);

    /**
     * @brief Quotes @p argument for a shell command line given to run().
     */
    std::string quote(std::string_view argument);

    /**
     * @brief Whether the docker client is installed and its daemon answers.
     *
     * @return Why docker cannot be used otherwise.
     */
    std::expected<void, std::string> checkAvailable();

    /**
     * @brief Pulls @p image unless it is there already.
     */
    std::expected<void, std::string> pullIfMissing(std::string_view image);

    /**
     * @brief Builds @p dockerfile in @p context as @p tag. Cached layers make a repeated build cheap.
     */
    std::expected<void, std::string>
    buildImage(std::string_view tag, std::filesystem::path const& dockerfile, std::filesystem::path const& context);

    /**
     * @brief A container name unique to this process: @p prefix, the pid and a counter.
     */
    std::string uniqueContainerName(std::string_view prefix);

    /**
     * @brief Removes the container @p name whether it runs or not, e.g. one whose owner was killed.
     */
    void removeContainer(std::string_view name);
}
