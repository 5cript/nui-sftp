#pragma once

#include <backend/file_tracking/instance_lock.hpp>

#include <string>

namespace FileTracking
{
    class TemporaryDirectoryInstance
    {
      public:
        TemporaryDirectoryInstance();
        ~TemporaryDirectoryInstance();

      private:
        std::string myInstanceId_;
        InstanceLock lock_;
    };
}