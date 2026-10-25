#pragma once

#include <docker/docker.hpp>

#include <expected>
#include <string>
#include <vector>

namespace Docker
{
    /**
     * @brief A detached container that is removed with this object.
     */
    class Container
    {
      public:
        struct Options
        {
            std::string image{};
            /** @brief The name is this prefix made unique, see uniqueContainerName(). */
            std::string namePrefix{"nui-sftp"};
            /** @brief Given to docker run before the image, e.g. --tmpfs or -e. */
            std::vector<std::string> runArguments{};
            /** @brief Replaces the command of the image when not empty. */
            std::vector<std::string> command{};
        };

        /**
         * @brief Starts a container with docker run --detach.
         */
        static std::expected<Container, std::string> start(Options const& options);

        ~Container();
        Container(Container const&) = delete;
        Container& operator=(Container const&) = delete;
        Container(Container&& other) noexcept;
        Container& operator=(Container&& other) noexcept;

        std::string const& name() const noexcept
        {
            return name_;
        }

        /**
         * @brief The address of the container on its docker network, reachable from the host without a published
         *        port.
         */
        std::expected<std::string, std::string> ipAddress() const;

        /**
         * @brief Runs @p command through sh inside the container, as @p user unless it is empty.
         */
        CommandResult execute(std::string_view command, std::string_view user = {}) const;

      private:
        explicit Container(std::string name);

      private:
        std::string name_;
    };
}
