#include <backend/file_tracking/instance_watch.hpp>

#include <utility>

namespace FileTracking
{
    struct InstanceWatch::Implementation
    {
        std::shared_ptr<efsw::FileWatcher> watcher;
        efsw::WatchID watchId;
        std::filesystem::path path;
    };

    InstanceWatch::InstanceWatch() = default;

    InstanceWatch::InstanceWatch(
        std::shared_ptr<efsw::FileWatcher> watcher,
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
    InstanceWatch& InstanceWatch::operator=(InstanceWatch&&) noexcept = default;

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
        if (impl_->watcher && impl_->watchId >= 0)
            impl_->watcher->removeWatch(impl_->watchId);
        impl_.reset();
    }
}
