#include <frontend/terminal/terminal_channel.hpp>
#include <frontend/terminal/channel_interface.hpp>
#include <frontend/terminal/shell_integration.hpp>
#include <log/log.hpp>
#include <frontend/nlohmann_compat.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/json.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <cctype>

// clang-format off
#ifdef NUI_INLINE
// @inline(js, xterm-js)
js_import { Terminal } from "@xterm/xterm";
js_import { FitAddon } from "@xterm/addon-fit";
js_import { SerializeAddon } from "@xterm/addon-serialize";
js_import { WebglAddon } from "@xterm/addon-webgl";
js_import { nanoid } from "nanoid";

globalThis.terminalUtility = {};
globalThis.terminalUtility.stringToUint8Array = (str) => {
    return new TextEncoder().encode(str);
};
globalThis.terminalUtility.terminals = new Map();
globalThis.terminalUtility.createTerminal = (host, options) => {

    let renderer = undefined;
    if (options.hasOwnProperty("renderer")) {
        if (options.renderer === "dom") {
            renderer = undefined;
        } else if (options.renderer === "webgl") {
            renderer = new WebglAddon();
        }
    }

    const defaultedOptions = {
        cursorBlink: true,
        fontSize: 14,
        fontFamily: "courier-new, courier, monospace",
        theme: {
            background: "#000000",
            foreground: "#FFFFFF",
        },
        ...options,
        cols: 80,
        rows: 30
    };

    const terminal = new Terminal(defaultedOptions);
    const addons = {
        fitAddon: new FitAddon(),
        rendererAddon: renderer,
        serializeAddon: new SerializeAddon()
    };
    for (const [key, value] of Object.entries(addons))
    {
        if (value)
            terminal.loadAddon(value);
    }

    const id = nanoid();

    // Resizing:
    // Lumino collapses inactive dock tabs to near-zero dimensions, which would
    // otherwise drive a refit that sends cols=0/rows=0 to the pty and clobbers
    // the user's stty settings. Guard by contentRect and offsetParent so only
    // genuine size changes propagate.
    const resizeObserver = new ResizeObserver((entries) => {
        globalThis.requestAnimationFrame(() => {
            if (!Array.isArray(entries) || !entries.length)
                return;

            const rect = entries[0].contentRect;
            if (rect.width <= 0 || rect.height <= 0)
                return;
            if (host.offsetParent === null)
                return;

            globalThis.terminalUtility.refitTerminal(id);
        });
    });
    addons.resizeObserver = resizeObserver;
    globalThis.terminalUtility.set(id, terminal, addons);

    const openAndObserve = () => {
        terminal.open(host);
        resizeObserver.observe(host);
        terminal.focus();
    };
    if (host.isConnected) {
        openAndObserve();
    } else {
        const wait = () => {
            if (host.isConnected) {
                openAndObserve();
            } else {
                requestAnimationFrame(wait);
            }
        };
        wait();
    }
    return id;
};
globalThis.terminalUtility.getTerminal = (id) => {
    if (!globalThis.terminalUtility.terminals.has(id))
        return undefined;
    const obtained = globalThis.terminalUtility.terminals.get(id);
    if (!obtained.hasOwnProperty("terminal"))
        return undefined;
    return obtained.terminal;
};
globalThis.terminalUtility.refitTerminal = (id) => {
    if (!globalThis.terminalUtility.terminals.has(id))
        return;

    const terminalStuff = globalThis.terminalUtility.get(id);
    terminalStuff.addons.fitAddon.fit();
};
globalThis.terminalUtility.disposeTerminal = (id) => {
    if (!globalThis.terminalUtility.terminals.has(id))
        return;

    const terminalStuff = globalThis.terminalUtility.get(id);
    terminalStuff.addons.resizeObserver.disconnect();
    terminalStuff.terminal.dispose();
    globalThis.terminalUtility.terminals.delete(id);
};
globalThis.terminalUtility.set = (id, terminal, addons) => {
    globalThis.terminalUtility.terminals.set(id, {
        terminal: terminal,
        addons: addons
    });
};
globalThis.terminalUtility.dumpTerminal = (id) => {
    const found = globalThis.terminalUtility.get(id);
    if (!found)
        return undefined;
    return found.addons.serializeAddon.serialize();
};
globalThis.terminalUtility.replayIntoTerminal = (id, text) => {
    const found = globalThis.terminalUtility.get(id);
    if (!found)
        return;
    found.terminal.write(text);
};
globalThis.terminalUtility.get = (id) => {
    if (!globalThis.terminalUtility.terminals.has(id))
        return undefined;
    return globalThis.terminalUtility.terminals.get(id);
};
// Shell integration: xterm's own parser is a correct, chunk safe OSC parser, so the sequences the
// preexec hooks emit are picked up here instead of scanning the byte stream ourselves. Returning
// true swallows the sequence so it never reaches the screen.
globalThis.terminalUtility.registerOscHandler = (id, code, cb) => {
    const terminal = globalThis.terminalUtility.getTerminal(id);
    if (!terminal)
        return undefined;
    return terminal.parser.registerOscHandler(code, (payload) => {
        cb(payload);
        return true;
    });
};
// @endinline

// @inline(css, xterm-js-css)
@import "../../node_modules/@xterm/xterm/css/xterm.css";
// @endinline
#endif
// clang-format on

namespace
{
    Nui::val terminalUtility()
    {
        return Nui::val::global("terminalUtility");
    }
    void debugPrintTerminalWrite(std::string const& data, bool isUserInput)
    {
        if (Log::level() > Log::Level::Debug)
            return;

        std::string debugPrint;
        for (auto c : data)
        {
            if (c == '\r')
            {
                debugPrint += "\\r";
            }
            else if (c == '\n')
            {
                debugPrint += "\\n";
            }
            else if (std::isprint(c))
            {
                debugPrint += c;
            }
            else
            {
                debugPrint += "\\x" + std::to_string(static_cast<int>(c));
            }
        }
        if (isUserInput)
        {
            // Never Log Here! Could contain sensitive data!
        }
        else
        {
            // Log::debug("Terminal::received", debugPrint);
        }
    }
}

struct TerminalChannel::Implementation
{
    Ids::ChannelId channelId{};
    std::string termId{};
    std::string command{};
    std::vector<std::pair<std::string, bool>> writeCache{};
    std::function<void(std::string const&, bool)> doWrite{};
    bool isLocked{false};
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput;
    Persistence::HistoryCaptureMode captureMode{Persistence::HistoryCaptureMode::off};
    std::function<void(std::string const&)> onCommandExecuted{};
    /**
     * @brief State of the filter that hides the shell integration bootstrap from the user.
     *
     * The bootstrap is written into the shell's stdin, so the tty echoes it right back at us like
     * any keystroke. We know byte for byte what we sent, so the echo is swallowed on its way to
     * xterm instead of being cleaned up afterwards. The filter is deliberately timid: the moment the
     * incoming bytes stop looking like our own echo it gives up and lets everything through, because
     * eating real output would be far worse than showing a stray line.
     */
    struct EchoSuppression
    {
        /// Exactly the bytes handed to writeUser, minus the trailing newline.
        std::string expected{};
        /// Bytes of a candidate match, kept back until it is clear whether they are our echo. They
        /// are released to xterm as soon as the candidate turns out to be real output.
        std::string held{};
        /// How far into `expected` the incoming bytes have matched.
        std::size_t matched{0};
        /// Bytes left to look at before the filter gives up. Guards against a shell that never
        /// echoes (`stty -echo`), where suppression would otherwise stay armed forever.
        std::size_t budget{0};
        /// True once `expected` matched completely and only the echo of the Enter is left.
        bool awaitingLineEnd{false};
        /// True while the bytes of an escape sequence are being swallowed.
        bool inEscapeSequence{false};
        bool active{false};
    } echoSuppression{};

    std::string filterBootstrapEcho(std::string const& data);
    /// Simple mode only: the line the user is typing, assembled from their own keystrokes.
    std::string typedLine{};
    /// Simple mode only: true while the bytes of an escape sequence (arrow keys, ...) are skipped.
    bool inEscapeSequence{false};
    // Lifetime sentinel: xterm onData/onResize callbacks capture a weak_ptr to this.
    // When the TerminalChannel is destroyed (e.g. by closeChannel after connection loss),
    // the shared_ptr is released and weak_ptr::lock() returns nullptr, so stale callbacks
    // become no-ops instead of dereferencing the freed object.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    // xterm's onData/onResize return an IDisposable. Holding those explicitly
    // and calling dispose() on teardown deregisters our Nui::bind functors
    // from xterm's event emitter — otherwise the emitter can keep the C++
    // std::function alive past the session's lifetime, and any late-fire
    // (e.g. a queued resize during widget detach) reaches a freed `this`.
    Nui::val onDataDisposable{Nui::val::undefined()};
    Nui::val onResizeDisposable{Nui::val::undefined()};
    /// Same story for the OSC 633 handler of the smart capture mode.
    Nui::val oscHandlerDisposable{Nui::val::undefined()};
    TerminalEngine* engine;

    Nui::val terminal() const
    {
        return terminalUtility().call<Nui::val>("getTerminal", termId);
    }

    ChannelInterface* channel()
    {
        return engine->channel(channelId);
    }

    void writeUser(std::string const& data)
    {
        if (isLocked)
        {
            onLockedUserInput(channelId, data);
            return;
        }

        if (auto* chan = channel(); chan)
        {
            chan->write(data);
        }
    }

    void writeRespectingCache(std::string const& data, bool isUserInput);
    void writeAfterCache(std::string const& data, bool isUserInput);
    void feedTypedLine(std::string const& data);

    Implementation(
        TerminalEngine* engine,
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput,
        Persistence::HistoryCaptureMode captureMode
    )
        : channelId{std::move(channelId)}
        , onLockedUserInput{std::move(onLockedUserInput)}
        , captureMode{captureMode}
        , engine{engine}
    {
        doWrite = [this](std::string const& data, bool isUserInput)
        {
            writeRespectingCache(data, isUserInput);
        };
    }
};

void TerminalChannel::Implementation::writeRespectingCache(std::string const& data, bool isUserInput)
{
    if (termId.empty())
    {
        if (!data.empty())
            writeCache.emplace_back(data, isUserInput);
        return;
    }

    // Switch writing mode to cacheless:
    doWrite = [this](std::string const& cacheData, bool cacheIsUserInput)
    {
        writeAfterCache(cacheData, cacheIsUserInput);
    };

    // Write out cache...
    if (!writeCache.empty())
    {
        for (auto const& [cacheData, cacheIsUserInput] : writeCache)
            doWrite(cacheData, cacheIsUserInput);
        writeCache.clear();
    }

    // Now write the new data:
    doWrite(data, isUserInput);
}

std::string TerminalChannel::Implementation::filterBootstrapEcho(std::string const& data)
{
    auto& state = echoSuppression;
    std::string output{};

    // The echo does not arrive first: the login banner and the prompt are still on their way when
    // the bootstrap is written. So the stream is passed through until the echo actually starts, and
    // only then swallowed. A candidate match holds its bytes back in `held` and releases them again
    // the moment it turns out not to be our line.
    const auto abandonCandidate = [&state, &output]() {
        output += state.held;
        state.held.clear();
        state.matched = 0;
        state.inEscapeSequence = false;
    };

    for (std::size_t index = 0; index < data.size(); ++index)
    {
        const auto character = data[index];

        if (state.budget == 0)
        {
            // The echo never came (a shell with echo turned off, a password prompt, ...). Whatever
            // is held back is real output and must be shown.
            abandonCandidate();
            state.active = false;
            return output + data.substr(index);
        }
        --state.budget;

        if (state.awaitingLineEnd)
        {
            // The Enter we sent comes back as CR, LF or both. Once it is gone, the prompt line still
            // holds the (now invisible) command, so it is cleared and the shell's next prompt lands
            // on it.
            if (character == '\n')
            {
                state.active = false;
                return output + "\r\x1b[2K" + data.substr(index + 1);
            }
            if (std::iscntrl(static_cast<unsigned char>(character)))
                continue;
            state.active = false;
            return output + data.substr(index);
        }

        if (state.matched > 0)
        {
            if (state.inEscapeSequence)
            {
                // Readline redraws the line while it "types" our bootstrap, so cursor movements are
                // interleaved with the echo.
                state.held += character;
                if (std::isalpha(static_cast<unsigned char>(character)) || character == '~')
                    state.inEscapeSequence = false;
                continue;
            }
            if (character == '\x1b')
            {
                state.held += character;
                state.inEscapeSequence = true;
                continue;
            }
            if (character == state.expected[state.matched])
            {
                state.held += character;
                ++state.matched;
                if (state.matched == state.expected.size())
                {
                    state.held.clear();
                    state.awaitingLineEnd = true;
                }
                continue;
            }
            // Wrapping breaks the echo across lines; those control bytes carry no text.
            if (std::iscntrl(static_cast<unsigned char>(character)))
            {
                state.held += character;
                continue;
            }

            // Not our line after all. Release what was held and reconsider this byte from scratch,
            // it may well be the real beginning of the echo.
            abandonCandidate();
            --index;
            continue;
        }

        if (character == state.expected[0])
        {
            state.held += character;
            state.matched = 1;
            continue;
        }

        output += character;
    }

    return output;
}

void TerminalChannel::Implementation::writeAfterCache(std::string const& data, bool isUserInput)
{
    if (data.empty())
        return;

    if (isUserInput)
    {
        writeUser(data);
    }
    else
    {
        const auto filtered = echoSuppression.active ? filterBootstrapEcho(data) : data;
        if (filtered.empty())
            return;

        std::string nlFixedData;
        bool previousWasCR = false;
        for (auto c : filtered)
        {
            if (c == '\n')
            {
                if (!previousWasCR)
                {
                    nlFixedData += '\r';
                }
                previousWasCR = false;
            }
            else if (c == '\r')
            {
                previousWasCR = true;
            }
            nlFixedData += c;
        }
        debugPrintTerminalWrite(nlFixedData, isUserInput);
        auto term = terminal();
        if (term.isUndefined())
        {
            Log::error("Failed to get terminal with id to write to it: '{}", termId);
            return;
        }
        term.call<void>("write", nlFixedData);
    }
}

// Simple mode: reconstruct the line the user is typing from their own keystrokes. This is the
// fallback for shells that cannot be instrumented, and it is wrong for everything that is not a
// plain prompt: editors, pagers, TUIs and history recall with the arrow keys all end up here as
// well. The captured line is whatever the user typed, not necessarily what the shell ran.
void TerminalChannel::Implementation::feedTypedLine(std::string const& data)
{
    for (const auto character : data)
    {
        if (inEscapeSequence)
        {
            // Escape sequences end on their final byte, a letter or a tilde.
            if (std::isalpha(static_cast<unsigned char>(character)) || character == '~')
                inEscapeSequence = false;
            continue;
        }

        switch (character)
        {
            case '\r':
            case '\n':
            {
                if (!typedLine.empty() && onCommandExecuted)
                    onCommandExecuted(typedLine);
                typedLine.clear();
                continue;
            }
            case '\x1b':
            {
                // Arrow keys, history recall and the like desynchronize the buffer from what the
                // shell has on its line, so the safe answer is to drop what was typed so far.
                inEscapeSequence = true;
                typedLine.clear();
                continue;
            }
            case '\x7f':
            case '\b':
            {
                if (!typedLine.empty())
                    typedLine.pop_back();
                continue;
            }
            case '\x03': // Ctrl-C
            case '\x04': // Ctrl-D
            case '\x15': // Ctrl-U
            {
                typedLine.clear();
                continue;
            }
            default:
                break;
        }

        if (std::isprint(static_cast<unsigned char>(character)) || static_cast<unsigned char>(character) >= 0x80)
            typedLine += character;
    }
}

TerminalChannel::TerminalChannel(
    TerminalEngine* engine,
    Ids::ChannelId channelId,
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput,
    Persistence::HistoryCaptureMode captureMode
)
    : impl_{
          std::make_unique<Implementation>(engine, std::move(channelId), std::move(onLockedUserInput), captureMode)
      }
{}
TerminalChannel::~TerminalChannel() = default;
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(TerminalChannel);

void TerminalChannel::connectionLossMode(bool isLocked)
{
    impl_->isLocked = isLocked;
}

std::string TerminalChannel::getAllTextContent() const
{
    const auto result = terminalUtility().call<Nui::val>("dumpTerminal", impl_->termId);
    if (!result.isString())
    {
        Log::error("Failed to dump terminal content for id: '{}'", impl_->termId);
        return "";
    }
    return result.as<std::string>();
}

void TerminalChannel::replayContent(std::string const& serializedDump)
{
    if (impl_->termId.empty())
    {
        Log::error("Cannot replay terminal content: no terminal id");
        return;
    }
    if (serializedDump.empty())
        return;
    terminalUtility().call<void>("replayIntoTerminal", impl_->termId, serializedDump);
}

std::string TerminalChannel::stealTerminal()
{
    const auto id = impl_->termId;
    impl_->termId.clear();
    return id;
}

void TerminalChannel::open(
    Nui::val host,
    Persistence::TerminalOptions const& options,
    std::function<void(bool, std::string const&)> onOpen
)
{
    if (isOpen())
        return;

    Log::info("Opening terminal channel");

    impl_->termId = terminalUtility().call<std::string>("createTerminal", host, asVal(options));

    auto term = impl_->terminal();
    if (term.isUndefined())
    {
        Log::error("Failed to get terminal with id: '{}", impl_->termId);
        dispose(
            [onOpen = std::move(onOpen)]()
            {
                onOpen(false, "Failed to get terminal");
            }
        );
        return;
    }

    Log::info("Channel terminal opened with id: '{}'", impl_->termId);

    impl_->onDataDisposable = term.call<Nui::val>(
        "onData",
        Nui::bind(
            [this, aliveWeak = std::weak_ptr<bool>(impl_->alive)](Nui::val data)
            {
                if (!aliveWeak.lock())
                    return;
                const auto asString = data.as<std::string>();
                if (impl_->captureMode == Persistence::HistoryCaptureMode::simple)
                    impl_->feedTypedLine(asString);
                write(asString, true);
            },
            std::placeholders::_1
        )
    );

    if (impl_->captureMode == Persistence::HistoryCaptureMode::smart)
    {
        impl_->oscHandlerDisposable = terminalUtility().call<Nui::val>(
            "registerOscHandler",
            impl_->termId,
            ShellIntegration::oscCode,
            Nui::bind(
                [this, aliveWeak = std::weak_ptr<bool>(impl_->alive)](Nui::val payload)
                {
                    if (!aliveWeak.lock())
                        return;
                    if (!payload.isString())
                        return;
                    const auto command = ShellIntegration::commandFromOscPayload(payload.as<std::string>());
                    if (command && impl_->onCommandExecuted)
                        impl_->onCommandExecuted(*command);
                },
                std::placeholders::_1
            )
        );
    }

    impl_->onResizeDisposable = term.call<Nui::val>(
        "onResize",
        Nui::bind(
            [this, aliveWeak = std::weak_ptr<bool>(impl_->alive)](Nui::val obj)
            {
                if (!aliveWeak.lock())
                    return;
                if (impl_->isLocked)
                    return;
                if (auto* channel = impl_->channel(); channel)
                    channel->resize(obj["cols"].as<int>(), obj["rows"].as<int>());
            },
            std::placeholders::_1
        )
    );

    if (!impl_->writeCache.empty())
        write("", false);

    onOpen(true, "");
}

void TerminalChannel::write(std::string const& data, bool isUserInput)
{
    impl_->doWrite(data, isUserInput);
}
void TerminalChannel::writeStderr(std::string const& data, bool isUserInput)
{
    impl_->doWrite(data, isUserInput);
}
void TerminalChannel::focus()
{
    if (!isOpen())
        return;
    auto term = impl_->terminal();
    if (term.isUndefined())
    {
        Log::error("Failed to get terminal with id to focus it: '{}", impl_->termId);
        return;
    }
    term.call<void>("focus");
}
void TerminalChannel::dispose(std::function<void()> onComplete, bool closeBackendChannel)
{
    if (impl_->termId.empty())
        return onComplete();

    auto cleanupFrontendChannel = [this, onComplete = std::move(onComplete)]()
    {
        // Deregister our Nui::bind onData / onResize listeners FIRST so xterm
        // can't fire them during its own dispose and so the JS emitter drops
        // its last reference to the C++ std::function — otherwise the functor
        // stays alive past the session, its `this` capture becomes dangling,
        // and the next stray event call crashes with
        // `getWasmTableEntry(index) is not a function`.
        if (!impl_->onDataDisposable.isUndefined() && !impl_->onDataDisposable.isNull())
        {
            impl_->onDataDisposable.call<void>("dispose");
            impl_->onDataDisposable = Nui::val::undefined();
        }
        if (!impl_->onResizeDisposable.isUndefined() && !impl_->onResizeDisposable.isNull())
        {
            impl_->onResizeDisposable.call<void>("dispose");
            impl_->onResizeDisposable = Nui::val::undefined();
        }
        if (!impl_->oscHandlerDisposable.isUndefined() && !impl_->oscHandlerDisposable.isNull())
        {
            impl_->oscHandlerDisposable.call<void>("dispose");
            impl_->oscHandlerDisposable = Nui::val::undefined();
        }

        auto term = impl_->terminal();
        if (term.isUndefined())
        {
            Log::error("Failed to get terminal with id to dispose it: '{}", impl_->termId);
            onComplete();
            return;
        }
        Log::info("Disposing terminal channel with id: '{}'", impl_->termId);
        terminalUtility().call<void>("disposeTerminal", impl_->termId);
        impl_->termId.clear();
        impl_->channelId.invalidate();
        onComplete();
    };

    if (impl_->engine && closeBackendChannel)
    {
        Log::info("Disposing channel backend for channel id: '{}'", impl_->channelId.value());
        impl_->engine->closeChannel(
            impl_->channelId,
            [cleanupFrontendChannel = std::move(cleanupFrontendChannel)]()
            {
                cleanupFrontendChannel();
            }
        );
    }
    else
    {
        Log::info("Disposing channel without closing backend channel for id: '{}'", impl_->termId);
        cleanupFrontendChannel();
    }
}

bool TerminalChannel::isOpen() const
{
    return !impl_->termId.empty();
}

void TerminalChannel::setOnCommandExecuted(std::function<void(std::string const&)> onCommandExecuted)
{
    impl_->onCommandExecuted = std::move(onCommandExecuted);
}

void TerminalChannel::installShellIntegration(std::string const& bootstrapLine)
{
    if (impl_->captureMode != Persistence::HistoryCaptureMode::smart)
        return;
    if (bootstrapLine.empty())
        return;

    // Goes through the same path a keystroke takes, so it reaches the shell's stdin no matter
    // whether the transport is ssh or a local pty. The leading space keeps the line out of the
    // shell's own history where the shell is configured to ignore space prefixed commands.
    const auto line = " " + bootstrapLine;

    // Arm the echo filter before writing, the tty may answer faster than the next statement runs.
    // The budget covers the login banner and the prompt that arrive before the echo, plus the cursor
    // movements readline pads it with. It is finite so a shell that never echoes cannot leave the
    // filter armed for the rest of the session.
    impl_->echoSuppression = Implementation::EchoSuppression{
        .expected = line,
        .matched = 0,
        .budget = line.size() * 8 + 8192,
        .awaitingLineEnd = false,
        .inEscapeSequence = false,
        .active = true,
    };

    write(line + "\n", true);
}
