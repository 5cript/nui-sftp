#pragma once

#include <remote_io/async/unfinished_task_result.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_stream.hpp>
#include <paths/remote_path.hpp>
#include <utility/describe.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace RemoteIo
{
    /**
     * @brief How a file is opened, combined with operator|.
     */
    enum class OpenMode : unsigned
    {
        Read = 1u,
        Write = 2u,
        Create = 4u,
        Truncate = 8u,
        Exclusive = 16u,
    };

    constexpr OpenMode operator|(OpenMode lhs, OpenMode rhs)
    {
        return static_cast<OpenMode>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
    }

    /**
     * @brief Whether @p modes has every flag of @p mode.
     */
    constexpr bool contains(OpenMode modes, OpenMode mode)
    {
        return (static_cast<unsigned>(modes) & static_cast<unsigned>(mode)) == static_cast<unsigned>(mode);
    }

    /**
     * @brief The limits the server sets for transfers.
     */
    struct TransferLimits
    {
        /** @brief The most bytes one write may send, 0 for no limit. */
        std::uint64_t maxWriteLength{0};
    };

    /**
     * @brief Where a chain of symlinks ends.
     */
    struct DeepLinkResult
    {
        /** @brief The content of the link at the given path, as stored on the server. */
        std::string linkContent;
        /** @brief The end of the link chain. */
        Paths::RemotePath linkTarget;
        /** @brief The attributes of @ref linkTarget, empty when it does not exist. */
        std::optional<FileInformation> targetInfo;
    };

    /**
     * @brief Lists a directory.
     */
    struct ListDirectory
    {
        using Result = std::vector<FileInformation>;
        Paths::RemotePath path;
        /** @brief Also reads the literal and the target of every symlink, without it a symlink is listed as itself. */
        bool resolveLinks{true};
    };
    BOOST_DESCRIBE_STRUCT(ListDirectory, (), (path, resolveLinks))

    /**
     * @brief The attributes of a path, of the end of the chain when it is a symlink.
     */
    struct Stat
    {
        using Result = FileInformation;
        Paths::RemotePath path;
    };
    BOOST_DESCRIBE_STRUCT(Stat, (), (path))

    /**
     * @brief The attributes of a path, of the link itself when it is a symlink.
     */
    struct Lstat
    {
        using Result = FileInformation;
        Paths::RemotePath path;
    };
    BOOST_DESCRIBE_STRUCT(Lstat, (), (path))

    /**
     * @brief Creates a directory, fails when the path exists already.
     */
    struct CreateNewDirectory
    {
        using Result = void;
        Paths::RemotePath path;
        std::filesystem::perms permissions{std::filesystem::perms::owner_all};
    };
    BOOST_DESCRIBE_STRUCT(CreateNewDirectory, (), (path, permissions))

    /**
     * @brief Creates a directory unless a directory exists at the path already.
     */
    struct CreateDirectoryIfItDoesntExist
    {
        using Result = void;
        Paths::RemotePath path;
        std::filesystem::perms permissions{std::filesystem::perms::owner_all};
    };
    BOOST_DESCRIBE_STRUCT(CreateDirectoryIfItDoesntExist, (), (path, permissions))

    /**
     * @brief Creates an empty file unless one exists at the path already.
     */
    struct CreateEmptyFile
    {
        using Result = void;
        Paths::RemotePath path;
        std::filesystem::perms permissions{std::filesystem::perms::owner_read | std::filesystem::perms::owner_write};
    };
    BOOST_DESCRIBE_STRUCT(CreateEmptyFile, (), (path, permissions))

    struct RemoveFile
    {
        using Result = void;
        Paths::RemotePath path;
    };
    BOOST_DESCRIBE_STRUCT(RemoveFile, (), (path))

    /**
     * @brief Removes an empty directory.
     */
    struct RemoveEmptyDirectory
    {
        using Result = void;
        Paths::RemotePath path;
    };
    BOOST_DESCRIBE_STRUCT(RemoveEmptyDirectory, (), (path))

    /**
     * @brief Removes each path without recursing: a directory only when it is empty, anything else is unlinked. Stops
     *        at the first failure.
     */
    struct RemoveAll
    {
        using Result = void;
        std::vector<Paths::RemotePath> paths;
    };
    BOOST_DESCRIBE_STRUCT(RemoveAll, (), (paths))

    /**
     * @brief The directories that are not empty.
     */
    struct FilterOutEmptyDirectories
    {
        using Result = std::vector<Paths::RemotePath>;
        std::vector<Paths::RemotePath> directories;
    };
    BOOST_DESCRIBE_STRUCT(FilterOutEmptyDirectories, (), (directories))

    /**
     * @brief Renames a path. Whether an existing destination is replaced follows Capabilities::renameSemantics.
     */
    struct Rename
    {
        using Result = void;
        Paths::RemotePath source;
        Paths::RemotePath destination;
    };
    BOOST_DESCRIBE_STRUCT(Rename, (), (source, destination))

    /**
     * @brief Moves a regular file to a destination that must not exist, atomically through a hard link. Fails with
     *        AlreadyExists when the destination exists, and with Unsupported only when the destination is known to be
     *        free and the protocol has no hard links or the server refused the link. It never renames, after
     *        Unsupported the caller may move the file with Rename.
     */
    struct LinkIntoPlace
    {
        using Result = void;
        Paths::RemotePath source;
        Paths::RemotePath destination;
    };
    BOOST_DESCRIBE_STRUCT(LinkIntoPlace, (), (source, destination))

    /**
     * @brief Opens a file for a transfer. Fails with Busy while every connection the protocol transfers on is taken,
     *        the caller waits for wakeWhenFree and opens again.
     */
    struct OpenFile
    {
        using Result = std::weak_ptr<IRemoteFileStream>;
        Paths::RemotePath path;
        OpenMode mode{OpenMode::Read};
        /** @brief The permissions of a file the open creates. */
        std::filesystem::perms permissions{std::filesystem::perms::owner_read | std::filesystem::perms::owner_write};
        /**
         * @brief Called once, from any thread, after the open failed with Busy, when a connection became free. An
         *        open that did not fail with Busy drops it.
         */
        std::function<void()> wakeWhenFree{};
    };
    BOOST_DESCRIBE_STRUCT(OpenFile, (), (path, mode, permissions))

    /**
     * @brief Reads at most maximumSize bytes of a file, for a read outside the operation queue such as an ignore file.
     *        Never fails with Busy: the protocol reads it alongside the transfers that hold its connections.
     */
    struct ReadFile
    {
        using Result = std::string;
        Paths::RemotePath path;
        std::size_t maximumSize{0};
    };
    BOOST_DESCRIBE_STRUCT(ReadFile, (), (path, maximumSize))

    /**
     * @brief Sets when a path was last modified. Needs Capabilities::setModificationTime. Fails with Unsupported for a
     *        time the protocol cannot carry.
     */
    struct SetModificationTime
    {
        using Result = void;
        Paths::RemotePath path;
        std::chrono::system_clock::time_point modificationTime{};
    };
    BOOST_DESCRIBE_STRUCT(SetModificationTime, (), (path, modificationTime))

    /**
     * @brief The directory the server starts the user in, with a fallback when the server does not tell.
     */
    struct HomeDirectory
    {
        using Result = Paths::RemotePath;
    };
    BOOST_DESCRIBE_STRUCT(HomeDirectory, (), ())

    /**
     * @brief The directory the server starts the user in, as the server resolves it, without a fallback. Needs
     *        Capabilities::canonicalPath.
     */
    struct CanonicalHomeDirectory
    {
        using Result = Paths::RemotePath;
    };
    BOOST_DESCRIBE_STRUCT(CanonicalHomeDirectory, (), ())

    /**
     * @brief The canonical form of an existing path, with every symlink resolved. Needs Capabilities::canonicalPath.
     */
    struct CanonicalPath
    {
        using Result = Paths::RemotePath;
        Paths::RemotePath path;
    };
    BOOST_DESCRIBE_STRUCT(CanonicalPath, (), (path))

    struct ReadTransferLimits
    {
        using Result = TransferLimits;
    };
    BOOST_DESCRIBE_STRUCT(ReadTransferLimits, (), ())

    /**
     * @brief Follows a symlink to the end of its chain. Needs Capabilities::symlinks.
     */
    struct ReadLinkDeep
    {
        using Result = DeepLinkResult;
        Paths::RemotePath path;
        /** @brief How many links the chain may have at most. */
        int maxDepth{10};
    };
    BOOST_DESCRIBE_STRUCT(ReadLinkDeep, (), (path, maxDepth))

    /**
     * @brief The literal of a symlink, valid UTF-8. Needs Capabilities::symlinks.
     */
    struct ReadLink
    {
        using Result = std::string;
        Paths::RemotePath path;
    };
    BOOST_DESCRIBE_STRUCT(ReadLink, (), (path))

    /**
     * @brief Creates a symlink whose content is the target, which may be relative. Needs Capabilities::symlinks.
     */
    struct CreateSymLink
    {
        using Result = void;
        std::string target;
        Paths::RemotePath linkPath;
    };
    BOOST_DESCRIBE_STRUCT(CreateSymLink, (), (target, linkPath))

    template <typename... OperationTs>
    struct OperationList
    {
        static constexpr std::size_t size = sizeof...(OperationTs);

        template <typename OperationT>
        static consteval std::size_t indexOf()
        {
            static_assert((std::is_same_v<OperationT, OperationTs> || ...), "Not an operation of the list");
            constexpr std::array matches{std::is_same_v<OperationT, OperationTs>...};
            return static_cast<std::size_t>(std::ranges::find(matches, true) - matches.begin());
        }
    };

    /**
     * @brief Every operation of a remote file system. Each implementation runs every one of them.
     */
    using RemoteFileSystemOperations = OperationList<
        ListDirectory,
        Stat,
        Lstat,
        CreateNewDirectory,
        CreateDirectoryIfItDoesntExist,
        CreateEmptyFile,
        RemoveFile,
        RemoveEmptyDirectory,
        RemoveAll,
        FilterOutEmptyDirectories,
        Rename,
        LinkIntoPlace,
        OpenFile,
        ReadFile,
        SetModificationTime,
        HomeDirectory,
        CanonicalHomeDirectory,
        CanonicalPath,
        ReadTransferLimits,
        ReadLinkDeep,
        ReadLink,
        CreateSymLink>;

    template <typename OperationT>
    using OperationResult = std::expected<typename OperationT::Result, RemoteError>;

    /**
     * @brief Hands over a result exactly once: the first one, later ones are ignored, or the unfinished result when it
     *        is destroyed before, because whatever makes the result never started or whoever ran it dropped it.
     */
    template <typename ResultT>
    class ResultCompletion
    {
      public:
        explicit ResultCompletion(std::function<void(ResultT)> complete)
            : complete_{std::move(complete)}
        {}
        ~ResultCompletion()
        {
            if (complete_)
                complete_(UnfinishedTaskResult<ResultT>{}());
        }
        ResultCompletion(ResultCompletion const&) = delete;
        ResultCompletion& operator=(ResultCompletion const&) = delete;
        ResultCompletion(ResultCompletion&& other) noexcept
            : complete_{std::exchange(other.complete_, nullptr)}
        {}
        ResultCompletion& operator=(ResultCompletion&&) = delete;

        void operator()(ResultT result)
        {
            if (complete_)
                std::exchange(complete_, nullptr)(std::move(result));
        }

      private:
        std::function<void(ResultT)> complete_;
    };

    /**
     * @brief Completes an operation exactly once, see ResultCompletion: unfinished when the operation never started or
     *        whoever ran it dropped it.
     */
    template <typename OperationT>
    using Completion = ResultCompletion<OperationResult<OperationT>>;

    /**
     * @brief An operation on its way to the file system, with what to do with its result.
     */
    template <typename OperationT>
    struct Pending
    {
        OperationT operation;
        Completion<OperationT> complete;
    };

    /**
     * @brief Whether a path of @p operation is unset, which must never reach the server.
     */
    template <typename OperationT>
    bool hasUnsetPath(OperationT const& operation)
    {
        bool unset = false;
        boost::mp11::mp_for_each<boost::describe::describe_members<OperationT, boost::describe::mod_public>>(
            [&operation, &unset](auto member)
            {
                auto const& value = operation.*member.pointer;
                using ValueT = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<ValueT, Paths::RemotePath>)
                    unset = unset || value.empty();
                else if constexpr (std::is_same_v<ValueT, std::vector<Paths::RemotePath>>)
                    unset = unset || std::ranges::any_of(value, &Paths::RemotePath::empty);
            }
        );
        return unset;
    }
}
