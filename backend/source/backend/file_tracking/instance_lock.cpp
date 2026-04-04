#include <backend/file_tracking/instance_lock.hpp>

namespace FileTracking
{
    void InstanceLock::init()
    {
        if (!lockFilePath_.empty())
            aquireLock();
    }
}