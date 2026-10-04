#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Test
{
    /**
     * @brief Test double for xterm's OSC parser: collects the payloads of one OSC code from a byte
     *        stream, chunk safe.
     *
     * In production xterm does this job; the headless xterm test feeds the same sequences to the real
     * parser and keeps this double honest. It mirrors xterm.js where it matters for us: C0 controls
     * inside the string are dropped, CAN and SUB abort the sequence, BEL or ESC end it (the backslash
     * of an ESC \ terminator is then a harmless escape of its own).
     */
    class OscScanner
    {
      public:
        explicit OscScanner(int code)
            : prefix_{std::to_string(code) + ";"}
        {}

        void feed(std::string_view data)
        {
            for (const auto character : data)
                feed(character);
        }

        /**
         * @brief The payloads seen so far, without the "<code>;" prefix.
         */
        std::vector<std::string> const& payloads() const
        {
            return payloads_;
        }

      private:
        enum class State
        {
            Ground,
            Escape,
            OscString,
        };

        void feed(char character)
        {
            switch (state_)
            {
                case State::Ground:
                    if (character == '\x1b')
                        state_ = State::Escape;
                    return;
                case State::Escape:
                    if (character == ']')
                    {
                        body_.clear();
                        state_ = State::OscString;
                        return;
                    }
                    state_ = character == '\x1b' ? State::Escape : State::Ground;
                    return;
                case State::OscString:
                    if (character == '\x07')
                        return finish();
                    if (character == '\x1b')
                    {
                        finish();
                        state_ = State::Escape;
                        return;
                    }
                    if (character == '\x18' || character == '\x1a')
                    {
                        state_ = State::Ground;
                        return;
                    }
                    if (static_cast<unsigned char>(character) < 0x20)
                        return;
                    body_ += character;
                    return;
            }
        }

        void finish()
        {
            state_ = State::Ground;
            if (body_.starts_with(prefix_))
                payloads_.push_back(body_.substr(prefix_.size()));
        }

        std::string prefix_;
        State state_{State::Ground};
        std::string body_{};
        std::vector<std::string> payloads_{};
    };
}
