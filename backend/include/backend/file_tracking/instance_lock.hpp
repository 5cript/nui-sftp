#pragma once

#include <filesystem>
#include <memory>

namespace FileTracking
{
    class InstanceLock
    {
      public:
        explicit InstanceLock(std::filesystem::path const& lockFilePath);
        ~InstanceLock();

        InstanceLock(InstanceLock const&) = delete;
        InstanceLock(InstanceLock&&);
        InstanceLock& operator=(InstanceLock const&) = delete;
        InstanceLock& operator=(InstanceLock&&);

        bool hasLock() const;

        /**
         * @brief Non-throwing acquire attempt.
         *
         * Unlike the constructor, this method does not throw on failure. It uses a
         * non-blocking lock operation so it returns immediately regardless of
         * whether another process holds the lock.
         *
         * @return true  The lock was successfully acquired.
         * @return false The file is already locked by another process, or the file
         *               could not be opened.
         */
        bool tryAcquire();

        /**
         * @brief Probe whether another process currently holds an exclusive lock.
         *
         * Opens (creating if necessary) and attempts a non-blocking exclusive lock.
         * If the attempt succeeds the lock is released immediately; the file is
         * otherwise unmodified. This is a static utility so it can be used without
         * constructing an InstanceLock.
         *
         * @param lockFilePath Path to the lock file to probe.
         * @return true  Another process holds an exclusive lock on the file.
         * @return false No other process holds the lock (or the file cannot be opened).
         */
        static bool isLockedByAnother(std::filesystem::path const& lockFilePath);

      private:
        bool aquireLock();
        void releaseLock();
        void init();

      private:
        std::filesystem::path lockFilePath_;

        struct SystemDependantData;
        std::unique_ptr<SystemDependantData> systemDependantData_;
    };
}