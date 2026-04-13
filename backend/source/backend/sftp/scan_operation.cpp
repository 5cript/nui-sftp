#include <backend/sftp/scan_operation.hpp>
#include <log/log.hpp>

#include <ssh/sftp_session.hpp>
#include <ssh/file_stream.hpp>

#include <string>
#include <algorithm>

ScanOperation::ScanOperation(SecureShell::SftpSession& sftp, ScanOperationOptions options)
    : sftp_(&sftp)
    , remotePath_{std::move(options.remotePath)}
    , progressCallback_{std::move(options.progressCallback)}
    , futureTimeout_{options.futureTimeout}
    , respectIgnoreFiles_{options.respectIgnoreFiles}
    , recursive_{options.recursive}
    , ignoreHidden_{options.ignoreHidden}
{}

ScanOperation::~ScanOperation() = default;

namespace
{
    std::optional<std::string>
    readRemoteFile(SecureShell::SftpSession& sftp, std::filesystem::path const& path, std::chrono::seconds timeout)
    {
        auto openFut = sftp.openFile(path, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        if (openFut.wait_for(timeout) != std::future_status::ready)
            return std::nullopt;
        auto openResult = openFut.get();
        if (!openResult.has_value())
            return std::nullopt;
        auto stream = openResult.value().lock();
        if (!stream)
            return std::nullopt;

        std::string contents;
        auto readFut = stream->readAll([&contents](std::string_view chunk) {
            contents.append(chunk);
            return true;
        });
        if (readFut.wait_for(timeout) != std::future_status::ready)
        {
            stream->close();
            return std::nullopt;
        }
        auto readResult = readFut.get();
        stream->close();
        if (!readResult.has_value())
            return std::nullopt;
        return contents;
    }
}

std::expected<std::vector<SharedData::DirectoryEntry>, ScanOperation::Error>
ScanOperation::scanner(std::filesystem::path const& path)
{
    if (!recursive_ && rootScanned_)
        return std::vector<SharedData::DirectoryEntry>{};
    rootScanned_ = true;

    auto fut = sftp_->listDirectory(path);
    fut.wait_for(futureTimeout_);
    if (fut.wait_for(futureTimeout_) != std::future_status::ready)
        return enterErrorState<std::vector<SharedData::DirectoryEntry>>({.type = ErrorType::FutureTimeout});

    auto result = fut.get();
    if (!result.has_value())
        return enterErrorState<std::vector<SharedData::DirectoryEntry>>(
            {.type = ErrorType::SftpError, .sftpError = result.error()}
        );

    auto entries = std::move(result).value();

    if (ignoreHidden_)
    {
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [](SharedData::DirectoryEntry const& entry) {
                const auto name = entry.path.filename().string();
                return !name.empty() && name.front() == '.';
            }),
            entries.end()
        );
    }

    if (!respectIgnoreFiles_)
        return {std::move(entries)};

    const auto relDir = [&]() -> std::filesystem::path {
        std::error_code errc{};
        auto rel = std::filesystem::relative(path, remotePath_, errc);
        if (errc || rel.empty() || rel == std::filesystem::path{"."})
            return {};
        return rel;
    }();

    for (auto const& entry : entries)
    {
        const auto name = entry.path.generic_string();
        if (name != ".gitignore" && name != ".ignore")
            continue;
        if (entry.type != SharedData::DirectoryEntry::FileType::Regular)
            continue;
        if (auto content = readRemoteFile(*sftp_, path / entry.path, futureTimeout_))
            ignoreMatcher_.addFile(relDir, *content);
    }

    if (ignoreMatcher_.empty())
        return {std::move(entries)};

    std::vector<SharedData::DirectoryEntry> filtered;
    filtered.reserve(entries.size());
    for (auto& entry : entries)
    {
        const auto childRel =
            relDir.empty() ? std::filesystem::path{entry.path} : (relDir / entry.path);
        const bool isDir = entry.type == SharedData::DirectoryEntry::FileType::Directory;
        if (ignoreMatcher_.isIgnored(childRel, isDir))
            continue;
        filtered.push_back(std::move(entry));
    }
    return {std::move(filtered)};
}

std::uint64_t ScanOperation::totalBytes() const
{
    return walker_->totalBytes();
}

std::expected<ScanOperation::WorkStatus, ScanOperation::Error> ScanOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case (NotStarted):
        {
            state_ = Running;
            Log::info("ScanOperation: Starting scan of '{}'.", remotePath_.generic_string());
            progressCallback_(0, 0, 0);
            return WorkStatus::MoreWork;
        }
        case (Running):
        {
            return withWalkerDo(
                [this](auto& walker) -> std::expected<ScanOperation::WorkStatus, ScanOperation::Error>
                {
                    if (walker.completed())
                    {
                        Log::info("ScanOperation: Scan of '{}' completed.", remotePath_.generic_string());
                        state_ = Completed;
                        return WorkStatus::Complete;
                    }

                    auto result = walker.walk();
                    if (!result.has_value())
                    {
                        Log::error("ScanOperation: Failed to scan directory: {}", result.error().toString());
                        return enterErrorState<WorkStatus>(result.error());
                    }
                    // -1, because the walker includes the base/root dir of the search:
                    progressCallback_(walker.totalBytes(), walker.currentIndex(), walker.totalEntries() - 1);
                    return WorkStatus::MoreWork;
                }
            );
        }
        case (Prepared):
        case (Preparing):
        case (Finalizing):
            Log::error("ScanOperation: Invalid state: {}", static_cast<int>(state_));
            return enterErrorState<WorkStatus>({.type = ErrorType::InvalidOperationState});
        case (Completed):
        {
            Log::warn("ScanOperation: Operation already completed.");
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("ScanOperation: Operation already failed.");
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("ScanOperation: Cannot work on canceled operation.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("ScanOperation: Operation completed with partial success.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, ScanOperation::Error> ScanOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info("ScanOperation: Scan of '{}' canceled.", remotePath_.generic_string());
        enterState(OperationState::Canceled);
    }
    return {};
}

SecureShell::ProcessingStrand* ScanOperation::strand() const
{
    return sftp_->strand();
}