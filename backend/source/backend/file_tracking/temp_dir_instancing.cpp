#include <backend/file_tracking/temp_dir_instancing.hpp>

#include <efsw/efsw.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>
#include <nui/rpc.hpp>
#include <ids/id.hpp>
#include <utility/enum_string_convert.hpp>
#include <log/log.hpp>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief Prepare the instance directory.
         *
         * Creates all required parent directories as a side effect so that the
         * result can be used immediately to construct an InstanceLock inside it.
         *
         * @param dir Path to create.
         * @return The same path that was passed in.
         * @throws std::runtime_error on filesystem failure.
         */
        std::filesystem::path prepareInstanceDir(std::filesystem::path const& dir)
        {
            std::error_code err;
            std::filesystem::create_directories(dir, err);
            if (err)
                throw std::runtime_error{"Failed to create instance directory: " + err.message()};
            return dir;
        }

        FileAction toFileAction(efsw::Action action)
        {
            switch (action)
            {
                case efsw::Actions::Add:
                    return FileAction::Added;
                case efsw::Actions::Modified:
                    return FileAction::Modified;
                case efsw::Actions::Delete:
                    return FileAction::Deleted;
                case efsw::Actions::Moved:
                    return FileAction::Moved;
                default:
                    return FileAction::Modified;
            }
        }
    } // namespace

    // -------------------------------------------------------------------------
    // FileChangeListener — efsw callback, runs on efsw's internal thread
    // -------------------------------------------------------------------------
    class FileChangeListener : public efsw::FileWatchListener
    {
      public:
        FileChangeListener(
            std::string instanceId,
            boost::asio::strand<boost::asio::any_io_executor> strand,
            Nui::RpcHub* hub
        )
            : instanceId_{std::move(instanceId)}
            , strand_{std::move(strand)}
            , hub_{hub}
            , lockFileName_{instanceId_ + ".lock"}
            , eventName_{"FileTracking::" + instanceId_ + "::onFileChanged"}
        {}

        void handleFileAction(
            efsw::WatchID /*watchId*/,
            const std::string& dir,
            const std::string& filename,
            efsw::Action action,
            std::string oldFilename
        ) override
        {
            if (filename == lockFileName_ || filename == "metadata.json")
                return;

            nlohmann::json payload = {
                {"action", Utility::enumToString(toFileAction(action))},
                {"directory", dir},
                {"filename", filename},
                {"oldFilename", oldFilename},
            };

            boost::asio::post(
                strand_,
                [hub = hub_, name = eventName_, pld = std::move(payload)]() mutable
                {
                    hub->callRemote(name, pld);
                }
            );
        }

      private:
        std::string instanceId_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        Nui::RpcHub* hub_;
        std::string lockFileName_;
        std::string eventName_;
    };

    // -------------------------------------------------------------------------
    // Implementation
    // -------------------------------------------------------------------------
    struct TemporaryDirectoryInstance::Implementation
    {
        Config config;
        std::string instanceId;
        std::filesystem::path instanceDir;
        InstanceLock lock;
        boost::asio::strand<boost::asio::any_io_executor> strand;
        Nui::RpcHub* hub;
        std::shared_ptr<efsw::FileWatcher> watcher;
        std::unique_ptr<FileChangeListener> listener;
        bool valid{false};

        /**
         * @brief Construct and initialise all members in dependency order.
         *
         * The directory must exist before the lock file inside it can be created,
         * which is why prepareInstanceDir() is called in the instanceDir initialiser.
         */
        Implementation(
            Config cfg,
            boost::asio::strand<boost::asio::any_io_executor> strd,
            Nui::RpcHub* hubPtr,
            std::string instId
        )
            : config{std::move(cfg)}
            , instanceId{std::move(instId)}
            , instanceDir{prepareInstanceDir(config.tempRootDir / instanceId)}
            , lock{instanceDir / (instanceId + ".lock")}
            , strand{std::move(strd)}
            , hub{hubPtr}
            , watcher{std::make_shared<efsw::FileWatcher>()}
            , listener{nullptr}
            , valid{false}
        {}
    };

    // -------------------------------------------------------------------------
    // TemporaryDirectoryInstance
    // -------------------------------------------------------------------------
    TemporaryDirectoryInstance::TemporaryDirectoryInstance(
        Config config,
        boost::asio::strand<boost::asio::any_io_executor> strand,
        Nui::Window& /*wnd*/,
        Nui::RpcHub& hub
    )
        : impl_{std::make_unique<Implementation>(
              std::move(config),
              std::move(strand),
              &hub,
              Ids::generateId().value()
          )}
    {
        impl_->listener = std::make_unique<FileChangeListener>(
            impl_->instanceId,
            impl_->strand,
            impl_->hub
        );

        writeMetadata();
        impl_->valid = true;
    }

    TemporaryDirectoryInstance::~TemporaryDirectoryInstance()
    {
        if (!impl_)
            return;

        // Stop the efsw thread before anything else so no more callbacks fire.
        impl_->watcher.reset();
        impl_->listener.reset();

        if (impl_->valid)
            writeDeadTimestamp();
    }

    std::string const& TemporaryDirectoryInstance::instanceId() const
    {
        return impl_->instanceId;
    }

    std::filesystem::path TemporaryDirectoryInstance::instanceDir() const
    {
        return impl_->instanceDir;
    }

    std::optional<InstanceWatch>
    TemporaryDirectoryInstance::addWatch(std::filesystem::path const& path, bool recursive)
    {
        namespace fs = std::filesystem;

        auto absPath = path.is_absolute() ? path : (impl_->instanceDir / path);
        absPath = fs::weakly_canonical(absPath);

        auto rel = fs::relative(absPath, impl_->instanceDir);
        if (rel.string().starts_with(".."))
        {
            Log::warn(
                "addWatch: '{}' is outside instance dir '{}'",
                absPath.string(),
                impl_->instanceDir.string()
            );
            return std::nullopt;
        }

        if (!fs::exists(absPath))
        {
            Log::warn("addWatch: path '{}' does not exist", absPath.string());
            return std::nullopt;
        }

        auto watchId = impl_->watcher->addWatch(absPath.string(), impl_->listener.get(), recursive);
        if (watchId < 0)
        {
            Log::warn("addWatch: efsw rejected watch for '{}'", absPath.string());
            return std::nullopt;
        }

        impl_->watcher->watch();
        return InstanceWatch{impl_->watcher, watchId, std::move(absPath)};
    }

    void TemporaryDirectoryInstance::cleanupNow()
    {
        if (!impl_)
            return;

        impl_->watcher.reset();
        impl_->listener.reset();

        std::error_code err;
        std::filesystem::remove_all(impl_->instanceDir, err);
        if (err)
            Log::warn("cleanupNow: failed to remove '{}': {}", impl_->instanceDir.string(), err.message());

        impl_->valid = false;
    }

    bool TemporaryDirectoryInstance::isValid() const
    {
        return impl_ && impl_->valid;
    }

    // ---- private helpers ----------------------------------------------------

    void TemporaryDirectoryInstance::writeMetadata()
    {
        auto const metaPath = impl_->instanceDir / "metadata.json";
        nlohmann::json meta = {
            {"instanceId", impl_->instanceId},
            {"createdAt", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"deadAt", nullptr},
        };
        std::ofstream out{metaPath};
        out << meta.dump(4);
    }

    void TemporaryDirectoryInstance::writeDeadTimestamp()
    {
        auto const metaPath = impl_->instanceDir / "metadata.json";
        try
        {
            nlohmann::json meta;
            if (std::filesystem::exists(metaPath))
            {
                std::ifstream metaIn{metaPath};
                metaIn >> meta;
            }
            meta["deadAt"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::ofstream out{metaPath};
            out << meta.dump(4);
        }
        catch (std::exception const& exc)
        {
            Log::error(
                "writeDeadTimestamp: failed for instance '{}': {}", impl_->instanceId, exc.what()
            );
        }
    }
}
