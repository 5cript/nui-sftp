#pragma once

#include <string>

namespace NuiFileExplorer
{
    /**
     * @brief What went wrong. The file explorer leaves the wording to the model, which may localize it.
     */
    enum class ErrorCode
    {
        /// Items were dragged onto the side they came from, which does not support moving or copying within it.
        DropOnSameSide,
        /// The drag and drop payload could not be read. The detail holds the parser error.
        DropDataUnreadable,
        /**
         * @brief Items were dropped or pasted onto a directory whose name is not valid UTF-8. The detail holds the
         *        name as shown.
         */
        DropTargetNameInvalid,
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
