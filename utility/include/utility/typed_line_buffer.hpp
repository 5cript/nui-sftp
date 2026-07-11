#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace Utility
{
    /**
     * @brief Simple history capture: reconstructs the line the user is typing from their keystrokes.
     *
     * The fallback for shells that cannot be instrumented. It is wrong for everything that is not a
     * plain prompt: editors, pagers, TUIs and history recall with the arrow keys all end up here as
     * well. The captured line is whatever the user typed, not necessarily what the shell ran.
     */
    class TypedLineBuffer
    {
      public:
        /**
         * @param onLine Receives every non empty line when Enter is pressed.
         */
        explicit TypedLineBuffer(std::function<void(std::string const&)> onLine);

        /**
         * @brief Feeds keystrokes as the terminal reports them; any chunking gives the same lines.
         */
        void feed(std::string_view data);

      private:
        std::function<void(std::string const&)> onLine_;
        std::string line_{};
        /// True while the bytes of an escape sequence (arrow keys, ...) are skipped.
        bool inEscapeSequence_{false};
        /// The bytes of the current escape sequence after the ESC.
        std::string escapeSequence_{};
        /// True between the bracketed paste markers, where a newline is part of the line.
        bool inPaste_{false};
    };
}
