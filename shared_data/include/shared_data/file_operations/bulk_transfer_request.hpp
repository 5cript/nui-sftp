#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace SharedData
{
    /**
     * @brief One local item of a bulk upload. A directory is scanned and uploaded with its content.
     */
    struct BulkUploadEntry
    {
        Paths::LocalPath source{};
        Paths::RemotePath destination{};
        /** @brief Trusted for files, the backend does not stat them. */
        std::uint64_t sizeBytes{0};
        bool isDirectory{false};
        /**
         * @brief A directory that is created without its content, as an entry of the files' bulk instead of a bulk
         *        of its own that scans it.
         */
        bool withoutContent{false};
        /**
         * @brief The user agreed to replace this existing destination. Without it, or the request's allowOverwrite,
         *        a destination that appeared after the existence check is not replaced.
         */
        bool mayOverwrite{false};
    };
    BOOST_DESCRIBE_STRUCT(
        BulkUploadEntry,
        (),
        (source, destination, sizeBytes, isDirectory, withoutContent, mayOverwrite)
    )

    /**
     * @brief One remote item of a bulk download. A directory is scanned and downloaded with its content.
     */
    struct BulkDownloadEntry
    {
        Paths::RemotePath source{};
        Paths::LocalPath destination{};
        /** @brief Trusted for files, the backend does not stat them. */
        std::uint64_t sizeBytes{0};
        bool isDirectory{false};
        /** @brief See @ref BulkUploadEntry::withoutContent. */
        bool withoutContent{false};
        /** @brief Modification time of the remote file, restored on the local copy so the next sync sees no change. */
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        /** @brief See @ref BulkUploadEntry::mayOverwrite. */
        bool mayOverwrite{false};
    };
    BOOST_DESCRIBE_STRUCT(
        BulkDownloadEntry,
        (),
        (source, destination, sizeBytes, isDirectory, withoutContent, mtime, mtimeNsec, mayOverwrite)
    )

    /**
     * @brief Single-RPC payload for uploading many items. Files become one aggregate bulk, every directory becomes
     *        a bulk of its own that scans first.
     */
    struct BulkUploadRequest
    {
        std::vector<BulkUploadEntry> entries{};
        bool allowOverwrite{false};
        bool insertRefresh{false};
        OperationMode mode{OperationMode::Queued};
        /**
         * @brief The directories the files' bulk works below, the labels of its card. Unset, the closest directories
         *        holding every source and every destination.
         */
        std::optional<Paths::LocalPath> localRoot{};
        std::optional<Paths::RemotePath> remoteRoot{};
    };
    BOOST_DESCRIBE_STRUCT(BulkUploadRequest, (), (entries, allowOverwrite, insertRefresh, mode, localRoot, remoteRoot))

    /**
     * @brief Download analogue of @ref BulkUploadRequest.
     */
    struct BulkDownloadRequest
    {
        std::vector<BulkDownloadEntry> entries{};
        bool allowOverwrite{false};
        bool insertRefresh{false};
        OperationMode mode{OperationMode::Queued};
        /**
         * @brief The directories the files' bulk works below, the labels of its card. No link may lie between the
         *        local one and a destination. Unset, the closest directories holding every source and every
         *        destination.
         */
        std::optional<Paths::LocalPath> localRoot{};
        std::optional<Paths::RemotePath> remoteRoot{};
    };
    BOOST_DESCRIBE_STRUCT(
        BulkDownloadRequest,
        (),
        (entries, allowOverwrite, insertRefresh, mode, localRoot, remoteRoot)
    )
}
