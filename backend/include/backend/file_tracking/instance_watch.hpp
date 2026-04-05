#pragma once

#include <efsw/efsw.hpp>

#include <filesystem>
#include <memory>

namespace FileTracking
{
    /**
     * @brief Move-only RAII handle representing one efsw directory watch.
     *
     * Destroying or moving-from an InstanceWatch automatically removes the
     * underlying efsw watch via efsw::FileWatcher::removeWatch().
     * Instances of this type are produced by
     * TemporaryDirectoryInstance::addWatch().
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
         * @param watcher  Shared owning pointer to the efsw::FileWatcher that
         *                 registered this watch.
         * @param watchId  The watch ID returned by efsw::FileWatcher::addWatch().
         * @param path     The directory path being watched.
         */
        InstanceWatch(
            std::shared_ptr<efsw::FileWatcher> watcher,
            efsw::WatchID watchId,
            std::filesystem::path path
        );

        ~InstanceWatch();

        InstanceWatch(InstanceWatch const&) = delete;
        InstanceWatch& operator=(InstanceWatch const&) = delete;

        InstanceWatch(InstanceWatch&&) noexcept;
        InstanceWatch& operator=(InstanceWatch&&) noexcept;

        /**
         * @brief The path that was passed to addWatch().
         */
        std::filesystem::path const& path() const;

        /**
         * @brief The raw efsw watch identifier.
         */
        efsw::WatchID watchId() const;

        /**
         * @brief Returns true when this handle references a live watch.
         */
        bool isValid() const;

        /**
         * @brief Explicitly remove the watch and invalidate this handle.
         *
         * Called automatically by the destructor.  Safe to call more than once.
         */
        void release();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
