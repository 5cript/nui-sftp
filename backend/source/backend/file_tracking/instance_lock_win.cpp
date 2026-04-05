#include <backend/file_tracking/instance_lock.hpp>

#include <utility>

#include <windows.h>

// Windows implementation
namespace FileTracking
{
    struct InstanceLock::SystemDependantData
    {
        HANDLE fileHandle{INVALID_HANDLE_VALUE};
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
        return systemDependantData_ && systemDependantData_->fileHandle != INVALID_HANDLE_VALUE;
    }
    bool InstanceLock::aquireLock()
    {
        systemDependantData_->fileHandle = CreateFileW(
            lockFilePath_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (systemDependantData_->fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        // Lock entire file (blocking - waits until lock is available)
        OVERLAPPED overlapped{}; // Note: NOT using LOCKFILE_FAIL_IMMEDIATELY flag for blocking behavior
        if (LockFileEx(systemDependantData_->fileHandle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped))
            return true;

        CloseHandle(systemDependantData_->fileHandle);
        systemDependantData_->fileHandle = INVALID_HANDLE_VALUE;
        return false;
    }
    void InstanceLock::releaseLock()
    {
        if (systemDependantData_->fileHandle != INVALID_HANDLE_VALUE)
        {
            OVERLAPPED overlapped{};
            UnlockFileEx(systemDependantData_->fileHandle, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(systemDependantData_->fileHandle);
            systemDependantData_->fileHandle = INVALID_HANDLE_VALUE;
        }
    }

    bool InstanceLock::tryAcquire()
    {
        systemDependantData_->fileHandle = CreateFileW(
            lockFilePath_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (systemDependantData_->fileHandle == INVALID_HANDLE_VALUE)
            return false;

        OVERLAPPED ovl{};
        if (!LockFileEx(
                systemDependantData_->fileHandle,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0,
                MAXDWORD,
                MAXDWORD,
                &ovl))
        {
            CloseHandle(systemDependantData_->fileHandle);
            systemDependantData_->fileHandle = INVALID_HANDLE_VALUE;
            return false;
        }
        return true;
    }

    bool InstanceLock::isLockedByAnother(std::filesystem::path const& lockFilePath)
    {
        HANDLE hdl = CreateFileW(
            lockFilePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hdl == INVALID_HANDLE_VALUE)
            return false;

        OVERLAPPED ovl{};
        bool locked =
            !LockFileEx(hdl, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ovl);
        if (!locked)
        {
            OVERLAPPED unlockOvl{};
            UnlockFileEx(hdl, 0, MAXDWORD, MAXDWORD, &unlockOvl);
        }
        CloseHandle(hdl);
        return locked;
    }
}