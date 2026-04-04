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