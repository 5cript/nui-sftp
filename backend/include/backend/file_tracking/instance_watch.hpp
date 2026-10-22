#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace FileTracking
{
    class DirectoryWatchService;

    /**
     * @brief Move-only RAII handle representing one subscription of a DirectoryWatchService.
     *
     * Destroying an InstanceWatch or assigning another one to it ends the
     * subscription. It does not keep the service alive: once the service is gone
     * there is nothing left to end. Instances of this type are produced by
     * DirectoryWatchService::subscribe().
     */
    class InstanceWatch
    {
      public:
        /**
         * @brief Construct an invalid (sentinel) watch.
         */
        InstanceWatch();

        /**
         * @brief Construct a valid watch handle.
         *
         * @param service  The service that holds the subscription.
         * @param watchId  The subscription.
         * @param path     The directory path being watched.
         */
        InstanceWatch(std::weak_ptr<DirectoryWatchService> service, std::int64_t watchId, std::filesystem::path path);

        ~InstanceWatch();

        InstanceWatch(InstanceWatch const&) = delete;
        InstanceWatch& operator=(InstanceWatch const&) = delete;

        InstanceWatch(InstanceWatch&&) noexcept;
        InstanceWatch& operator=(InstanceWatch&&) noexcept;

        /**
         * @brief The path that was passed to subscribe().
         */
        std::filesystem::path const& path() const;

        /**
         * @brief The subscription, negative for an invalid handle.
         */
        std::int64_t watchId() const;

        /**
         * @brief Returns true when this handle references a live watch.
         */
        bool isValid() const;

        /**
         * @brief Explicitly end the subscription and invalidate this handle.
         *
         * Called automatically by the destructor.  Safe to call more than once.
         */
        void release();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
