#pragma once

#include <ftp/protocol/listing.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

namespace FileTransferProtocol::Detail
{
    /**
     * @brief What one line of a listing holds.
     */
    struct ListingLine
    {
        /** @brief The entry, nothing for a line that lists no child, like "total 12" or a cdir entry of MLSD. */
        std::optional<ListingEntry> entry{};
        /** @brief Facts whose value could not be read. */
        std::size_t unreadableFacts{0u};
    };

    /**
     * @brief What becomes of the cdir and pdir entries of MLSx.
     */
    enum class CurrentAndParent
    {
        /** @brief MLSD: they are no children. */
        Skip,
        /** @brief MLST: the entry of a directory. */
        AsDirectory,
    };

    /**
     * @brief Reads a line of MLSx without its line end, and for MLST without its leading space.
     *
     * @return Nothing for a line that breaks the grammar or names a fact twice.
     */
    std::optional<ListingLine> readMachineListingLine(std::string_view line, CurrentAndParent currentAndParent);

    /**
     * @brief Reads a line of LIST without its line end, in Unix or DOS format, see ListingParser::list().
     *
     * @return Nothing for a line in neither format.
     */
    std::optional<ListingLine> readListLine(std::string_view line, std::chrono::sys_days today);
}
