#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(
        OperationType,
        Scan,
        LocalScan,
        Download,
        BulkDownload,
        Upload,
        BulkUpload,
        Rename,
        Delete,
        BulkDelete,
        LocalDelete,
        LocalCopy,
        LocalMove,
        ArchiveDownload,
        ArchiveUpload,
        CustomAction
    )

    /**
     * @brief True for a local copy or move.
     */
    inline bool isLocalTransfer(OperationType type)
    {
        return type == OperationType::LocalCopy || type == OperationType::LocalMove;
    }

    /**
     * @brief True for the operations of the local operation queue, which runs apart from the sessions.
     */
    inline bool isLocalOperation(OperationType type)
    {
        return type == OperationType::LocalDelete || isLocalTransfer(type);
    }
}