#pragma once

#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>

namespace FileTracking
{
    /**
     * @brief The texts of the pure parts in the current language, from the "fileTracking" group of the language files.
     *        WASM only: the pure headers never include the language system.
     */
    FileTrackingTexts translatedTexts();
}
