#include <frontend/terminal/terminal_channel.hpp>
#include <frontend/terminal/channel_interface.hpp>
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

    terminal.open(host);
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
    addons.resizeObserver.observe(host);

    terminal.focus();
    globalThis.terminalUtility.set(id, terminal, addons);
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
globalThis.terminalUtility.get = (id) => {
    if (!globalThis.terminalUtility.terminals.has(id))
        return undefined;
    return globalThis.terminalUtility.terminals.get(id);
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
    // Lifetime sentinel: xterm onData/onResize callbacks capture a weak_ptr to this.
    // When the TerminalChannel is destroyed (e.g. by closeChannel after connection loss),
    // the shared_ptr is released and weak_ptr::lock() returns nullptr, so stale callbacks
    // become no-ops instead of dereferencing the freed object.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
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

    Implementation(
        TerminalEngine* engine,
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : channelId{std::move(channelId)}
        , onLockedUserInput{std::move(onLockedUserInput)}
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
        std::string nlFixedData;
        bool previousWasCR = false;
        for (auto c : data)
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

TerminalChannel::TerminalChannel(
    TerminalEngine* engine,
    Ids::ChannelId channelId,
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
)
    : impl_{std::make_unique<Implementation>(engine, std::move(channelId), std::move(onLockedUserInput))}
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

    term.call<void>(
        "onData",
        Nui::bind(
            [this, aliveWeak = std::weak_ptr<bool>(impl_->alive)](Nui::val data, Nui::val)
            {
                if (!aliveWeak.lock())
                    return;
                write(data.as<std::string>(), true);
            },
            std::placeholders::_1,
            std::placeholders::_2
        )
    );

    term.call<void>(
        "onResize",
        Nui::bind(
            [this, aliveWeak = std::weak_ptr<bool>(impl_->alive)](Nui::val obj, Nui::val)
            {
                if (!aliveWeak.lock())
                    return;
                if (impl_->isLocked)
                    return;
                // Log::debug("Terminal resized {}:{}. ", obj["cols"].as<int>(), obj["rows"].as<int>());
                if (auto* channel = impl_->channel(); channel)
                    channel->resize(obj["cols"].as<int>(), obj["rows"].as<int>());
            },
            std::placeholders::_1,
            std::placeholders::_2
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
