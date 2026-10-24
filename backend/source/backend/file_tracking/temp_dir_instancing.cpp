#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <backend/file_tracking/instance_bookkeeping.hpp>
#include <backend/file_tracking/directory_watcher.hpp>
#include <backend/file_tracking/change_reporter.hpp>
#include <backend/file_tracking/change_filter.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <paths/native.hpp>
#include <persistence/state/sftp_options.hpp>

#include <nlohmann/json.hpp>
#include <ids/id.hpp>
#include <log/log.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief Creates the instance directory with its bookkeeping directory, so the lock can be created in it.
         *
         * @param dir Path to create.
         * @return The same path that was passed in.
         * @throws std::runtime_error on filesystem failure.
         */
        std::filesystem::path prepareInstanceDir(std::filesystem::path const& dir)
        {
            if (const auto created = createBookkeepingDirectory(dir); !created)
                throw std::runtime_error{fmt::format("Failed to create instance directory: {}", created.error())};
            return dir;
        }
    } // namespace

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
        std::optional<DirectoryWatcher> watcher;
        bool valid{false};

        /**
         * @brief Construct and initialise all members in dependency order.
         *
         * The directory must exist before the lock file inside it can be created,
         * which is why prepareInstanceDir() is called in the instanceDir initialiser.
         */
        Implementation(Config cfg, boost::asio::strand<boost::asio::any_io_executor> strd, std::string instId)
            : config{std::move(cfg)}
            , instanceId{std::move(instId)}
            , instanceDir{prepareInstanceDir(config.tempRootDir / instanceId)}
            , lock{std::in_place, bookkeepingPaths(instanceDir).lock}
            , strand{std::move(strd)}
            , watcher{}
            , valid{false}
        {}
    };

    // -------------------------------------------------------------------------
    // TemporaryDirectoryInstance
    // -------------------------------------------------------------------------
    TemporaryDirectoryInstance::TemporaryDirectoryInstance(
        Config config,
        boost::asio::strand<boost::asio::any_io_executor> strand
    )
        : impl_{std::make_unique<Implementation>(std::move(config), std::move(strand), Ids::generateId().value())}
    {
        writeMetadata();
        impl_->valid = true;
    }

    TemporaryDirectoryInstance::~TemporaryDirectoryInstance()
    {
        if (!impl_)
            return;

        // Stop the change handler before anything else so no more changes are reported.
        impl_->watcher.reset();

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
        std::string downloadTempSuffix,
        ChangeSink sink
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

        // The first watch decides the suffix and the sink, later ones share its event stream.
        if (!impl_->watcher)
            impl_->watcher.emplace(
                impl_->strand,
                [reporter = ChangeReporter{impl_->strand, std::move(sink), downloadTempSuffix},
                 watchedInstanceDirectory = fs::weakly_canonical(impl_->instanceDir)](DirectoryChange const& change)
                {
                    if (!isBookkeepingPath(watchedInstanceDirectory, change.path()))
                        reporter.report(change);
                }
            );
        // A download writes its temporary many times over, those events go no further than efsw's thread.
        return impl_->watcher->add(
            absPath,
            recursive,
            [suffix = Persistence::effectiveTempFileSuffix(downloadTempSuffix)](DirectoryEvent const& event)
            {
                return !isTransientChange(event.filename, event.oldFilename, suffix);
            }
        );
    }

    std::expected<void, std::string> TemporaryDirectoryInstance::recordOrigin(Paths::RemotePath const& remotePath)
    {
        Log::debug("recordOrigin: not recording '{}' for instance '{}' yet", remotePath.value(), impl_->instanceId);
        return {};
    }

    void TemporaryDirectoryInstance::cleanupNow()
    {
        if (!impl_)
            return;

        impl_->watcher.reset();
        impl_->lock.reset();

        std::error_code err;
        Utility::removeAllNoFollow(impl_->instanceDir, err);
        if (err)
            Log::warn(
                "cleanupNow: failed to remove '{}': {}",
                Paths::nativeToUtf8(impl_->instanceDir),
                Utility::errorMessage(err)
            );

        impl_->valid = false;
    }

    bool TemporaryDirectoryInstance::isValid() const
    {
        return impl_ && impl_->valid;
    }

    // ---- private helpers ----------------------------------------------------

    void TemporaryDirectoryInstance::writeMetadata()
    {
        const nlohmann::json meta = {
            {"instanceId", impl_->instanceId},
            {"createdAt", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"deadAt", nullptr},
        };
        if (const auto written = FileTracking::writeMetadata(impl_->instanceDir, meta); !written)
            Log::error("writeMetadata: failed for instance '{}': {}", impl_->instanceId, written.error());
    }

    void TemporaryDirectoryInstance::writeDeadTimestamp()
    {
        const auto written =
            readMetadata(impl_->instanceDir)
                .and_then(
                    [this](std::optional<nlohmann::json> meta)
                    {
                        auto updated = std::move(meta).value_or(nlohmann::json{{"instanceId", impl_->instanceId}});
                        updated["deadAt"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                        return FileTracking::writeMetadata(impl_->instanceDir, updated);
                    }
                );
        if (!written)
            Log::error("writeDeadTimestamp: failed for instance '{}': {}", impl_->instanceId, written.error());
    }
}
