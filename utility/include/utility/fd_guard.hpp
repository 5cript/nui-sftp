#pragma once

#include <unistd.h>

namespace Utility
{
    /**
     *  @brief Owning POSIX file descriptor. Closes on destruction; non-copyable, movable.
     *  @details Lets callers ::open() a file once and pass the guard around without risking
     *           a double-close or a leak on an early return. Move-only by design -- if you
     *           need to share an fd, dup() it explicitly first.
     */
    class FdGuard
    {
      public:
        FdGuard() noexcept = default;
        explicit FdGuard(int fd) noexcept
            : fd_{fd}
        {}
        ~FdGuard() noexcept
        {
            reset();
        }
        FdGuard(FdGuard const&) = delete;
        FdGuard& operator=(FdGuard const&) = delete;
        FdGuard(FdGuard&& other) noexcept
            : fd_{other.fd_}
        {
            other.fd_ = -1;
        }
        FdGuard& operator=(FdGuard&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        [[nodiscard]] int get() const noexcept
        {
            return fd_;
        }
        [[nodiscard]] bool valid() const noexcept
        {
            return fd_ != -1;
        }

        /**
         *  @brief Relinquish ownership of the fd to the caller; the guard becomes empty
         *         and will not close it. Use when transferring ownership across an API
         *         boundary that takes responsibility for closing.
         */
        [[nodiscard]] int release() noexcept
        {
            const int fd = fd_;
            fd_ = -1;
            return fd;
        }

        void reset(int fd = -1) noexcept
        {
            if (fd_ != -1)
                ::close(fd_);
            fd_ = fd;
        }

      private:
        int fd_{-1};
    };
}
