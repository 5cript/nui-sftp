#pragma once

#include <backend/file_tracking/directory_watcher.hpp>
#include <backend/file_tracking/reported_change.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <memory>
#include <string>

namespace FileTracking
{
    class SettledChanges;

    /**
     * @brief Reports the changes of one watch to its sink. Noise (see filterFileChange) is dropped, content changes of
     *        files are reported once they settled (see SettledChanges). Lives on its strand, copies share what is held
     *        back.
     */
    class ChangeReporter
    {
      public:
        /**
         * @param strand The strand report() is called on.
         * @param sink Receives the changes on @p strand.
         * @param downloadTempSuffix The suffix downloads write under until complete, see
         *        Persistence::effectiveTempFileSuffix.
         */
        ChangeReporter(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            ChangeSink sink,
            std::string const& downloadTempSuffix
        );

        void report(DirectoryChange const& change) const;

        /**
         * @brief Whether a change of @p filename is noise that report() drops, see isTransientFile.
         */
        bool isNoise(std::string const& filename) const;

      private:
        std::shared_ptr<SettledChanges> settledChanges_;
        std::string downloadTempSuffix_;
    };
}
