#include <backend/file_tracking/instance_watch.hpp>

#include <utility>

namespace FileTracking
{
    struct InstanceWatch::Implementation
    {
        std::weak_ptr<efsw::FileWatcher> watcher;
        efsw::WatchID watchId;
        std::filesystem::path path;
    };

    InstanceWatch::InstanceWatch() = default;

    InstanceWatch::InstanceWatch(
        std::weak_ptr<efsw::FileWatcher> watcher,
        efsw::WatchID watchId,
        std::filesystem::path path
    )
        : impl_{std::make_unique<Implementation>(Implementation{
              .watcher = std::move(watcher),
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

    efsw::WatchID InstanceWatch::watchId() const
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
        if (const auto watcher = impl_->watcher.lock(); watcher && impl_->watchId >= 0)
            watcher->removeWatch(impl_->watchId);
        impl_.reset();
    }
}
