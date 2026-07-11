#include <utility/echo_suppressor.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace Utility
{
    void EchoSuppressor::arm(std::string expectedLine, std::size_t passes)
    {
        const auto budget = (expectedLine.size() * 16 + 8192) * passes;
        arm(std::move(expectedLine), passes, budget);
    }

    void EchoSuppressor::arm(std::string expectedLine, std::size_t passes, std::size_t budget)
    {
        expected_ = std::move(expectedLine);
        held_.clear();
        positions_.clear();
        rewindAllowed_ = false;
        paddingPending_ = false;
        escape_ = Escape::None;
        budget_ = budget;
        awaitingLineEnd_ = false;
        remainingPasses_ = passes;
        active_ = !expected_.empty();
    }

    bool EchoSuppressor::active() const
    {
        return active_;
    }

    bool EchoSuppressor::advanceEscape(char character)
    {
        switch (escape_)
        {
            case Escape::None:
                return true;
            case Escape::Start:
                if (character == '[')
                {
                    escape_ = Escape::ControlSequence;
                    return false;
                }
                // OSC, DCS, APC, PM and SOS run until BEL or ST.
                if (character == ']' || character == 'P' || character == '_' || character == '^' || character == 'X')
                {
                    escape_ = Escape::String;
                    return false;
                }
                escape_ = Escape::None;
                return true;
            case Escape::ControlSequence:
                if (character >= 0x40 && character <= 0x7e)
                {
                    escape_ = Escape::None;
                    return true;
                }
                return false;
            case Escape::String:
                if (character == '\x07')
                {
                    escape_ = Escape::None;
                    return true;
                }
                if (character == '\x1b')
                    escape_ = Escape::StringEscape;
                return false;
            case Escape::StringEscape:
                escape_ = Escape::None;
                return true;
        }
        return true;
    }

    bool EchoSuppressor::advanceMatch(char character)
    {
        auto candidates = positions_;
        if (rewindAllowed_)
        {
            const auto furthest = *std::ranges::max_element(positions_);
            for (auto position = furthest > rewindWindow ? furthest - rewindWindow : 0; position <= furthest; ++position)
                candidates.push_back(position);
            // Blank cells may be skipped with a cursor movement instead of printing the spaces, as
            // fish does where it wraps the line between two words.
            for (auto position = furthest; position < expected_.size() && expected_[position] == ' '; ++position)
                candidates.push_back(position + 1);
        }

        std::vector<std::size_t> next;
        for (auto const position : candidates)
        {
            if (position < expected_.size() && expected_[position] == character)
                next.push_back(position + 1);
        }
        if (next.empty())
            return false;

        std::ranges::sort(next);
        next.erase(std::ranges::unique(next).begin(), next.end());
        if (next.size() > rewindWindow + 1)
            next.erase(next.begin(), next.end() - static_cast<std::ptrdiff_t>(rewindWindow + 1));
        positions_ = std::move(next);
        rewindAllowed_ = false;
        return true;
    }

    void EchoSuppressor::markCursorMovement()
    {
        rewindAllowed_ = true;
        paddingPending_ = false;
    }

    bool EchoSuppressor::matchedCompletely() const
    {
        return std::ranges::find(positions_, expected_.size()) != positions_.end();
    }

    void EchoSuppressor::resetCandidate()
    {
        held_.clear();
        positions_.clear();
        rewindAllowed_ = false;
        paddingPending_ = false;
        escape_ = Escape::None;
    }

    bool EchoSuppressor::holding() const
    {
        return active_ && !held_.empty();
    }

    std::string EchoSuppressor::release()
    {
        auto released = std::move(held_);
        resetCandidate();
        return released;
    }

    std::string EchoSuppressor::abandonCandidate(std::string& output)
    {
        auto replay = std::move(held_);
        resetCandidate();
        if (replay.empty())
            return replay;
        output += replay.front();
        replay.erase(0, 1);
        return replay;
    }

    std::string EchoSuppressor::filter(std::string_view chunk)
    {
        if (!active_)
            return std::string{chunk};

        std::string output{};
        // Bytes still to look at. A failed candidate puts its held bytes back in front, minus the
        // first: the real start of the echo may be hidden anywhere among them.
        std::string input{chunk};

        // The echo does not arrive first: the login banner and the prompt are still on their way when
        // the line is written. So the stream is passed through until the echo actually starts, and
        // only then swallowed. A candidate match holds its bytes back and releases them again the
        // moment it turns out not to be the line.
        for (std::size_t index = 0; index < input.size(); ++index)
        {
            const auto character = input[index];

            if (budget_ == 0)
            {
                // The echo never came (a shell with echo turned off, a password prompt, ...). Whatever
                // is held back is real output and must be shown.
                output += held_;
                held_.clear();
                active_ = false;
                output += input.substr(index);
                return output;
            }
            --budget_;

            if (awaitingLineEnd_)
            {
                // The Enter comes back as CR, LF or both, possibly after mode switches like the end of
                // bracketed paste. Once it is gone, the prompt line still holds the (now invisible)
                // command, so it is cleared and the shell's next prompt lands on it. The filter stays
                // armed for the next echo pass while one remains.
                if (escape_ != Escape::None)
                {
                    advanceEscape(character);
                    continue;
                }
                if (character == '\x1b')
                {
                    escape_ = Escape::Start;
                    continue;
                }
                if (character == '\n')
                {
                    output += "\r\x1b[2K";
                    awaitingLineEnd_ = false;
                    if (remainingPasses_ == 0 || --remainingPasses_ == 0)
                    {
                        active_ = false;
                        output += input.substr(index + 1);
                        return output;
                    }
                    continue;
                }
                if (std::iscntrl(static_cast<unsigned char>(character)))
                    continue;
                awaitingLineEnd_ = false;
                if (remainingPasses_ == 0 || --remainingPasses_ == 0)
                {
                    active_ = false;
                    output += input.substr(index);
                    return output;
                }
                --index;
                continue;
            }

            if (positions_.empty())
            {
                // A redraw may position the cursor past the leading blanks instead of printing them.
                const auto firstVisible = expected_.find_first_not_of(' ');
                if (character == expected_[0])
                    positions_.push_back(1);
                if (firstVisible != std::string::npos && firstVisible > 0 && character == expected_[firstVisible])
                    positions_.push_back(firstVisible + 1);
                if (!positions_.empty())
                {
                    held_ += character;
                    if (matchedCompletely())
                    {
                        held_.clear();
                        positions_.clear();
                        awaitingLineEnd_ = true;
                    }
                    continue;
                }
                output += character;
                continue;
            }

            if (escape_ != Escape::None)
            {
                held_ += character;
                if (advanceEscape(character))
                    markCursorMovement();
                continue;
            }
            if (character == '\x1b')
            {
                held_ += character;
                escape_ = Escape::Start;
                continue;
            }
            if (std::iscntrl(static_cast<unsigned char>(character)))
            {
                held_ += character;
                markCursorMovement();
                continue;
            }

            if (!paddingPending_ && advanceMatch(character))
            {
                held_ += character;
                if (matchedCompletely())
                {
                    held_.clear();
                    positions_.clear();
                    rewindAllowed_ = false;
                    awaitingLineEnd_ = true;
                }
                continue;
            }
            if (!paddingPending_ && character == ' ')
            {
                held_ += character;
                paddingPending_ = true;
                continue;
            }

            // Not the line after all. Release the first held byte and look at the rest again, this
            // byte included; the real beginning of the echo may be among them.
            const auto replay = abandonCandidate(output);
            input.insert(index, replay);
            --index;
        }

        return output;
    }
}
