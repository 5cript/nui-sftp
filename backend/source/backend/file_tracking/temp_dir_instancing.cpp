#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <backend/rpc_helper.hpp>
#include <utility/link_safe_remove.hpp>
#include <paths/native.hpp>
#include <backend/file_tracking/change_filter.hpp>

#include <efsw/efsw.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>
#include <nui/rpc.hpp>
#include <ids/id.hpp>
#include <utility/enum_string_convert.hpp>
#include <log/log.hpp>

#include <chrono>
#include <fstream>
#include <optional>
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
            Nui::RpcHub* hub,
            std::string downloadTempSuffix
        )
            : instanceId_{std::move(instanceId)}
            , strand_{std::move(strand)}
            , hub_{hub}
            , lockFileName_{instanceId_ + ".lock"}
            , eventName_{"FileTracking::" + instanceId_ + "::onFileChanged"}
            , downloadTempSuffix_{Persistence::effectiveTempFileSuffix(downloadTempSuffix)}
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

            std::error_code statusError;
            // efsw reports UTF-8 on every platform.
            const bool isDirectory = std::filesystem::is_directory(
                Paths::nativeFromUtf8(dir) / Paths::nativeFromUtf8(filename), statusError
            );
            const auto change = filterFileChange(
                {toFileAction(action), filename, std::move(oldFilename)}, isDirectory, downloadTempSuffix_
            );
            if (!change)
                return;

            nlohmann::json payload = {
                {"action", Utility::enumToString(change->action)},
                {"directory", Paths::nativeToUtf8(Paths::nativeFromUtf8(dir))},
                {"filename", Paths::nativeToUtf8(Paths::nativeFromUtf8(change->filename))},
                {"oldFilename", Paths::nativeToUtf8(Paths::nativeFromUtf8(change->oldFilename))},
            };

            boost::asio::post(
                strand_,
                [hub = hub_, name = eventName_, pld = std::move(payload)]() mutable
                {
                    RpcHelper::callRemoteSafely(*hub, name, pld);
                }
            );
        }

      private:
        std::string instanceId_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        Nui::RpcHub* hub_;
        std::string lockFileName_;
        std::string eventName_;
        std::string downloadTempSuffix_;
    };

    // -------------------------------------------------------------------------
    // Implementation
    // -------------------------------------------------------------------------
    struct TemporaryDirectoryInstance::Implementation
    {
        Config config;
        std::string instanceId;
        std::filesystem::path instanceDir;
        std::optional<InstanceLock> lock;
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
            , lock{std::in_place, instanceDir / (instanceId + ".lock")}
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
        : impl_{std::make_unique<Implementation>(std::move(config), std::move(strand), &hub, Ids::generateId().value())}
    {
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

    std::optional<InstanceWatch> TemporaryDirectoryInstance::addWatch(
        std::filesystem::path const& path,
        bool recursive,
        std::string downloadTempSuffix
    )
    {
        namespace fs = std::filesystem;

        auto absPath = path.is_absolute() ? path : (impl_->instanceDir / path);
        absPath = fs::weakly_canonical(absPath);

        auto rel = fs::relative(absPath, impl_->instanceDir);
        if (rel.empty() || *rel.begin() == "..")
        {
            Log::warn(
                "addWatch: '{}' is outside instance dir '{}'",
                Paths::nativeToUtf8(absPath),
                Paths::nativeToUtf8(impl_->instanceDir)
            );
            return std::nullopt;
        }

        if (!fs::exists(absPath))
        {
            Log::warn("addWatch: path '{}' does not exist", Paths::nativeToUtf8(absPath));
            return std::nullopt;
        }

        if (impl_->listener)
        {
            Log::error("Watch already exists for instance '{}'", impl_->instanceId);
            return std::nullopt;
        }

        impl_->listener = std::make_unique<FileChangeListener>(
            impl_->instanceId, impl_->strand, impl_->hub, std::move(downloadTempSuffix)
        );

        // efsw takes UTF-8, a narrow string is the ANSI code page on Windows.
        auto watchId = impl_->watcher->addWatch(Paths::nativeToUtf8(absPath), impl_->listener.get(), recursive);
        if (watchId < 0)
        {
            Log::warn("addWatch: efsw rejected watch for '{}'", Paths::nativeToUtf8(absPath));
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
        impl_->lock.reset();

        std::error_code err;
        Utility::removeAllNoFollow(impl_->instanceDir, err);
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
            Log::error("writeDeadTimestamp: failed for instance '{}': {}", impl_->instanceId, exc.what());
        }
    }
}
