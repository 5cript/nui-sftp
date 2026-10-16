#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

namespace Utility
{
    namespace Detail
    {
        /**
         * @brief True for a Windows reparse tag that makes its item a link: a name surrogate such as a symlink
         *        (IO_REPARSE_TAG_SYMLINK), a junction (IO_REPARSE_TAG_MOUNT_POINT) or a WSL symlink
         *        (IO_REPARSE_TAG_LX_SYMLINK). Cloud file placeholders, deduplicated files and AF_UNIX sockets are
         *        reparse points too, but no links.
         */
        bool isLinkReparseTag(std::uint32_t tag) noexcept;
    }

    /**
     * @brief What an item is, inspected without following it.
     */
    enum class NoFollowKind
    {
        Missing,
        /** @brief A symlink or, on Windows, a junction or another name surrogate reparse point. */
        Link,
        Directory,
        /** @brief A regular file or any other item that is no directory and no link. */
        Other
    };

    /**
     * @brief Inspects @p path without following it. A missing path is no error.
     */
    NoFollowKind noFollowKindOf(std::filesystem::path const& path, std::error_code& error);

    /**
     * @brief True for a symlink and, on Windows, for a junction or another reparse point whose tag is a name
     *        surrogate (see Detail::isLinkReparseTag). std::filesystem may report a junction as a plain directory,
     *        and walking into it leaves the tree it belongs to.
     */
    bool isLinkOrJunction(std::filesystem::path const& path, std::error_code& error);

    /**
     * @brief Directories already found to be real directories, so @ref firstLinkBelow inspects each of them once.
     *        One set belongs to one operation, which writes below a single root. Thread safe.
     */
    class VerifiedDirectories
    {
      public:
        bool contains(std::filesystem::path const& directory) const;
        void insert(std::filesystem::path const& directory);

      private:
        mutable std::mutex mutex_{};
        std::set<std::filesystem::path> directories_{};
    };

    /**
     * @brief Finds the first existing component of @p path below @p root (root excluded, @p path included) that is
     *        a symlink or junction. Writing through it would leave @p root. Components after the first missing one
     *        are not inspected, they cannot exist.
     * @param verified Directories inspected before, skipped here; the real directories found are added to it.
     * @return The offending component, or nothing when there is none. @p error is set when @p path is not lexically
     *         below @p root or a component cannot be inspected.
     */
    std::optional<std::filesystem::path> firstLinkBelow(
        std::filesystem::path const& root,
        std::filesystem::path const& path,
        std::error_code& error,
        VerifiedDirectories* verified = nullptr
    );

    /**
     * @brief Removes @p path itself without following it: a file, a link (never its target) or an empty directory.
     *        On Windows a read-only item has its read-only attribute cleared and is removed again.
     * @return True when @p path was removed, false when it did not exist or @p error is set.
     */
    bool removeNoFollow(std::filesystem::path const& path, std::error_code& error);

    /**
     * @brief An item that could not be removed, and why.
     */
    struct RemoveFailure
    {
        std::filesystem::path path;
        std::error_code error;
    };

    /**
     * @brief The outcome of @ref removeAllNoFollow.
     */
    struct RemoveAllResult
    {
        /** @brief Every entry removed, the given path included. */
        std::uintmax_t removed{0};
        /** @brief The items that stopped the removal, by their own path. Their parent directories stay as well. */
        std::vector<RemoveFailure> failures{};
    };

    /**
     * @brief Removes @p path and, for a real directory, everything below it. A symlink or junction is removed
     *        itself and never followed, so nothing outside @p path is touched. An item that cannot be removed does
     *        not stop the others, only the directories containing it stay.
     */
    RemoveAllResult removeAllNoFollow(std::filesystem::path const& path);

    /**
     * @brief @ref removeAllNoFollow that sets @p error to the error of the first item that could not be removed.
     * @return The number of removed entries, 0 when @p path does not exist.
     */
    std::uintmax_t removeAllNoFollow(std::filesystem::path const& path, std::error_code& error);
}
