#pragma once

#include <string>

namespace NuiFileExplorer
{
    /**
     * @brief What went wrong. The file explorer leaves the wording to the model, which may localize it.
     */
    enum class ErrorCode
    {
        /// External items were dropped onto the local side, which does not support it.
        ExternalDropOnLocalSide,
        /// Items were dragged onto the side they came from; moving within a side is not supported.
        DropOnSameSide,
        /// The drag and drop payload could not be read. The detail holds the parser error.
        DropDataUnreadable,
    };

    struct Error
    {
        ErrorCode code;

        /**
         * @brief Untranslated technical detail, may be empty.
         */
        std::string detail{};
    };
}
