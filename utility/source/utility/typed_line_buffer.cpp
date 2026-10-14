#include <utility/typed_line_buffer.hpp>

#include <paths/utf8.hpp>

#include <cctype>
#include <utility>

namespace Utility
{
    TypedLineBuffer::TypedLineBuffer(std::function<void(std::string const&)> onLine)
        : onLine_{std::move(onLine)}
    {}

    void TypedLineBuffer::feed(std::string_view data)
    {
        for (const auto character : data)
        {
            if (inEscapeSequence_)
            {
                // Escape sequences end on their final byte, a letter or a tilde.
                escapeSequence_ += character;
                if (!std::isalpha(static_cast<unsigned char>(character)) && character != '~')
                    continue;
                inEscapeSequence_ = false;
                // Bracketed paste markers frame text that belongs to the line.
                if (escapeSequence_ == "[200~")
                    inPaste_ = true;
                else if (escapeSequence_ == "[201~")
                    inPaste_ = false;
                else
                    // Arrow keys, history recall and the like desynchronize the buffer from what the
                    // shell has on its line, so the safe answer is to drop what was typed so far.
                    line_.clear();
                continue;
            }

            switch (character)
            {
                case '\r':
                case '\n':
                {
                    if (inPaste_)
                    {
                        line_ += '\n';
                        continue;
                    }
                    if (!line_.empty() && onLine_)
                        onLine_(line_);
                    line_.clear();
                    continue;
                }
                case '\x1b':
                {
                    inEscapeSequence_ = true;
                    escapeSequence_.clear();
                    continue;
                }
                case '\x7f':
                case '\b':
                {
                    if (!line_.empty())
                        line_.resize(Paths::truncatedUtf8(line_, line_.size() - 1u).size());
                    continue;
                }
                case '\x03': // Ctrl-C
                case '\x04': // Ctrl-D
                case '\x15': // Ctrl-U
                {
                    line_.clear();
                    continue;
                }
                default:
                    break;
            }

            if (std::isprint(static_cast<unsigned char>(character)) || static_cast<unsigned char>(character) >= 0x80)
                line_ += character;
        }
    }
}
