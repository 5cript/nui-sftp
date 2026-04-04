#include <backend/file_tracking/instance_lock.hpp>

#include <utility>
#include <fcntl.h>
#include <sys/file.h>

// Linux & Mac implementation
namespace FileTracking
{
    struct InstanceLock::SystemDependantData
    {
        int fileDescriptor{-1};
    };

    InstanceLock::InstanceLock(std::filesystem::path const& lockFilePath)
        : lockFilePath_(lockFilePath)
        , systemDependantData_(std::make_unique<SystemDependantData>())
    {
        if (!aquireLock())
            throw std::runtime_error("Failed to acquire instance lock");
    }

    InstanceLock::~InstanceLock()
    {
        if (lockFilePath_.empty() || !systemDependantData_)
            return;

        releaseLock();
    }

    InstanceLock::InstanceLock(InstanceLock&& other)
        : lockFilePath_(std::exchange(other.lockFilePath_, {}))
        , systemDependantData_(std::exchange(other.systemDependantData_, nullptr))
    {}

    InstanceLock& InstanceLock::operator=(InstanceLock&& other)
    {
        if (this != &other)
        {
            lockFilePath_ = std::exchange(other.lockFilePath_, {});
            systemDependantData_ = std::exchange(other.systemDependantData_, nullptr);
        }
        return *this;
    }

    bool InstanceLock::hasLock() const
    {
        return systemDependantData_->fileDescriptor != -1;
    }
    bool InstanceLock::aquireLock()
    {
        systemDependantData_->fileDescriptor = open(lockFilePath_.c_str(), O_RDWR | O_CREAT, 0666);
        if (systemDependantData_->fileDescriptor == -1)
        {
            // Failed to open lock file
            return false;
        }

        if (flock(systemDependantData_->fileDescriptor, LOCK_EX) == -1)
        {
            // Failed to acquire lock
            close(systemDependantData_->fileDescriptor);
            systemDependantData_->fileDescriptor = -1;
            return false;
        }
        return true;
    }
    void InstanceLock::releaseLock()
    {
        if (systemDependantData_->fileDescriptor != -1)
        {
            flock(systemDependantData_->fileDescriptor, LOCK_UN);
            close(systemDependantData_->fileDescriptor);
            systemDependantData_->fileDescriptor = -1;
        }
    }
}