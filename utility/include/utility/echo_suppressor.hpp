#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Utility
{
    /**
     * @brief Hides the tty echo of a line the program wrote into a shell itself, e.g. the shell
     *        integration bootstrap.
     *
     * The line goes into the shell's stdin, so the tty echoes it right back like any keystroke, and
     * the line editor redraws it again once it starts. The bytes written are known exactly, so the
     * echo is swallowed on its way to the terminal instead of being cleaned up afterwards.
     *
     * Line editors do not echo verbatim. Escape sequences and control bytes are interleaved, and
     * around a line wrap they print a padding space before moving the cursor (readline, ZLE) or
     * print the last characters again after moving it (readline, ZLE, fish), and blank cells may be
     * skipped by moving the cursor over them (fish). So after any cursor movement the match may
     * rewind a few characters or skip spaces, and one space right before a movement is tolerated.
     *
     * The filter is still deliberately timid: everything it holds back is released the moment the
     * bytes stop looking like the echo, because eating real output would be far worse than showing
     * a stray line.
     */
    class EchoSuppressor
    {
      public:
        /**
         * @brief Echoes swallowed by default: written before the shell reads, the line comes back
         *        from the tty's canonical mode echo and again from the line editor's redraw. See
         *        ShellIntegration::echoCount for shells that echo more often.
         */
        static constexpr std::size_t defaultPasses = 2;

        /**
         * @brief How far back the match may rewind after a cursor movement.
         */
        static constexpr std::size_t rewindWindow = 16;

        /**
         * @brief Starts swallowing the echo of @p expectedLine with a budget fit for the line.
         *
         * The budget covers the login banner and the prompt that arrive before the echo, plus the
         * cursor movements and redraws the line editor pads it with.
         *
         * @param expectedLine Exactly the bytes written, without the trailing newline.
         * @param passes How many echoes of the line are swallowed at most.
         */
        void arm(std::string expectedLine, std::size_t passes = defaultPasses);

        /**
         * @brief Starts swallowing the echo of @p expectedLine.
         *
         * @param expectedLine Exactly the bytes written, without the trailing newline.
         * @param passes How many echoes of the line are swallowed at most.
         * @param budget Bytes looked at before the filter gives up; guards against a shell that never
         *        echoes (`stty -echo`), where it would otherwise stay armed forever.
         */
        void arm(std::string expectedLine, std::size_t passes, std::size_t budget);

        /**
         * @brief Whether the filter still looks for the echo; once inactive, filter() is a no-op.
         */
        bool active() const;

        /**
         * @brief Returns what of @p chunk is shown; the echo is replaced by a cleared line.
         *
         * Bytes that might belong to the echo are held back until a later chunk decides, so the
         * output of one call can lag behind its input. See holding() and release().
         */
        std::string filter(std::string_view chunk);

        /**
         * @brief Whether bytes that might start the echo are held back, waiting for the next chunk.
         */
        bool holding() const;

        /**
         * @brief Gives up the current candidate and returns the bytes it held; the filter stays armed.
         *
         * For when the output went quiet. The echo arrives in one burst, so a candidate left waiting
         * is real output, like the space that ends a prompt while the line starts with a space.
         */
        std::string release();

      private:
        enum class Escape
        {
            None,
            Start,
            ControlSequence,
            String,
            StringEscape,
        };

        /**
         * @brief Feeds one byte of an escape sequence.
         *
         * @return Whether the sequence ended with it.
         */
        bool advanceEscape(char character);

        /**
         * @brief Moves the candidate match over a printable byte.
         *
         * @return Whether the byte fits the echo.
         */
        bool advanceMatch(char character);

        void markCursorMovement();
        bool matchedCompletely() const;
        void resetCandidate();
        /**
         * @brief Gives up the candidate: its first byte goes to @p output.
         *
         * @return The other held bytes, to be looked at again.
         */
        std::string abandonCandidate(std::string& output);

      private:
        std::string expected_{};
        /// Bytes of a candidate match, kept back until it is clear whether they are the echo.
        std::string held_{};
        /// Positions in expected_ the candidate may be at; empty while there is no candidate.
        std::vector<std::size_t> positions_{};
        /// Set by a cursor movement: the next printable byte may redraw recent characters.
        bool rewindAllowed_{false};
        /// A space that matched nothing; accepted as wrap padding if a cursor movement follows.
        bool paddingPending_{false};
        Escape escape_{Escape::None};
        std::size_t budget_{0};
        /// True once expected_ matched completely and only the echo of the Enter is left.
        bool awaitingLineEnd_{false};
        std::size_t remainingPasses_{0};
        bool active_{false};
    };
}
