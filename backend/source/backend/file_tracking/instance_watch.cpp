#include <backend/file_tracking/instance_watch.hpp>
#include <backend/file_tracking/directory_watch_service.hpp>

#include <utility>

namespace FileTracking
{
    struct InstanceWatch::Implementation
    {
        std::weak_ptr<DirectoryWatchService> service;
        std::int64_t watchId;
        std::filesystem::path path;
    };

    InstanceWatch::InstanceWatch() = default;

    InstanceWatch::InstanceWatch(
        std::weak_ptr<DirectoryWatchService> service,
        std::int64_t watchId,
        std::filesystem::path path
    )
        : impl_{std::make_unique<Implementation>(Implementation{
              .service = std::move(service),
              .watchId = watchId,
              .path = std::move(path),
          })}
    {}

    InstanceWatch::~InstanceWatch()
    {
        release();
    }

    InstanceWatch::InstanceWatch(InstanceWatch&&) noexcept = default;
    InstanceWatch& InstanceWatch::operator=(InstanceWatch&& other) noexcept
    {
        if (this != &other)
        {
            release();
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    std::filesystem::path const& InstanceWatch::path() const
    {
        static std::filesystem::path empty{};
        if (!impl_)
            return empty;
        return impl_->path;
    }

    std::int64_t InstanceWatch::watchId() const
    {
        if (!impl_)
            return -1;
        return impl_->watchId;
    }

    bool InstanceWatch::isValid() const
    {
        return impl_ != nullptr && impl_->watchId >= 0;
    }

    void InstanceWatch::release()
    {
        if (!impl_)
            return;
        if (const auto service = impl_->service.lock(); service && impl_->watchId >= 0)
            service->unsubscribe(impl_->watchId);
        impl_.reset();
    }
}
