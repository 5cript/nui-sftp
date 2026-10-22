#include <backend/file_tracking/change_reporter.hpp>
#include <backend/file_tracking/change_filter.hpp>
#include <backend/file_tracking/settled_changes.hpp>
#include <persistence/state/sftp_options.hpp>
#include <paths/native.hpp>

#include <utility>

namespace FileTracking
{
    ChangeReporter::ChangeReporter(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        ChangeSink sink,
        std::string const& downloadTempSuffix
    )
        : settledChanges_{std::make_shared<SettledChanges>(std::move(strand), std::move(sink))}
        , downloadTempSuffix_{Persistence::effectiveTempFileSuffix(downloadTempSuffix)}
    {}

    bool ChangeReporter::isNoise(std::string const& filename) const
    {
        return isTransientFile(filename, downloadTempSuffix_);
    }

    void ChangeReporter::report(DirectoryChange const& change) const
    {
        const auto filtered = filterFileChange(
            {change.action, change.filename, change.oldFilename}, change.isDirectory, downloadTempSuffix_
        );
        if (!filtered)
            return;

        const bool isFileContent =
            !change.isDirectory && (filtered->action == FileAction::Added || filtered->action == FileAction::Modified);
        settledChanges_->add(
            ReportedChange{
                .action = filtered->action,
                .path = change.directory / Paths::nativeFromUtf8(filtered->filename),
                .oldPath = filtered->oldFilename.empty()
                    ? std::filesystem::path{}
                    : change.directory / Paths::nativeFromUtf8(filtered->oldFilename),
                .isDirectory = change.isDirectory,
            },
            isFileContent
        );
    }
}
