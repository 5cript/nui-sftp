#pragma once

#include <filesystem>
#include <optional>
#include <utility>

namespace NuiFileExplorer
{
    /**
     * @brief An item of a directory to select once the listing of that directory has loaded.
     *
     * Moving to another directory forgets the request, so does the first listing of its directory, whether the item
     * is in it or not.
     */
    class PendingSelection
    {
      public:
        /**
         * @brief Remembers to select the item @p name of @p directory, replacing an earlier request.
         *
         * @param directory The directory as the side model shows it, after navigating there.
         * @param name The file name of the item in @p directory.
         */
        void request(std::filesystem::path directory, std::filesystem::path name)
        {
            request_ = Request{.directory = std::move(directory), .name = std::move(name)};
        }

        /**
         * @brief Forgets the request when @p path, the directory the side moved to, is not its directory.
         */
        void onPathChanged(std::filesystem::path const& path)
        {
            if (request_ && request_->directory != path)
                request_.reset();
        }

        /**
         * @brief Ends the request on a listing of @p listedDirectory.
         *
         * @return The full path of the item to select when @p listedDirectory is the directory of the request,
         *         nothing otherwise.
         */
        std::optional<std::filesystem::path> take(std::filesystem::path const& listedDirectory)
        {
            return std::exchange(request_, std::nullopt)
                .and_then(
                    [&listedDirectory](Request const& request) -> std::optional<std::filesystem::path>
                    {
                        if (request.directory != listedDirectory)
                            return std::nullopt;
                        return request.directory / request.name;
                    }
                );
        }

        bool isPending() const
        {
            return request_.has_value();
        }

      private:
        struct Request
        {
            std::filesystem::path directory;
            std::filesystem::path name;
        };

        std::optional<Request> request_{};
    };
}
