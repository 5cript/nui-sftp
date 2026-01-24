#include <frontend/terminal/frontend_ssh_manager.hpp>
#include <log/log.hpp>
#include <frontend/nlohmann_compat.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/json.hpp>
#include <nui/frontend/utility/functions.hpp>

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
    const resizeObserver = new ResizeObserver((entries) => {
        globalThis.requestAnimationFrame(() => {
            if (!Array.isArray(entries) || !entries.length)
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

struct GenericTerminalChannel
{
    Ids::ChannelId channelId{};
    std::string termId{};
    std::string command{};
    std::vector<std::pair<std::string, bool>> writeCache{};
    std::function<void(std::string const&, bool)> doWrite{};
    bool isLocked{false};
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput;

    Nui::val terminal() const
    {
        return terminalUtility().call<Nui::val>("getTerminal", termId);
    }

    virtual void writeUser(std::string const& data) = 0;

    void writeRespectingCache(std::string const& data, bool isUserInput);
    void writeAfterCache(std::string const& data, bool isUserInput);

    GenericTerminalChannel(
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : channelId{std::move(channelId)}
        , onLockedUserInput{std::move(onLockedUserInput)}
    {
        doWrite = [this](std::string const& data, bool isUserInput)
        {
            writeRespectingCache(data, isUserInput);
        };
    }

    virtual ~GenericTerminalChannel() = default;
};

struct TerminalChannel::Implementation : public GenericTerminalChannel
{
    MultiChannelTerminalEngine* engine;

    ChannelInterface* channel()
    {
        return engine->channel(channelId);
    }

    void writeUser(std::string const& data) override
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

    Implementation(
        MultiChannelTerminalEngine* engine,
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : GenericTerminalChannel{std::move(channelId), std::move(onLockedUserInput)}
        , engine{engine}
    {}
};

void GenericTerminalChannel::writeRespectingCache(std::string const& data, bool isUserInput)
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

void GenericTerminalChannel::writeAfterCache(std::string const& data, bool isUserInput)
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
    MultiChannelTerminalEngine* engine,
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
            [this](Nui::val data, Nui::val)
            {
                write(data.as<std::string>(), true);
            },
            std::placeholders::_1,
            std::placeholders::_2
        )
    );

    term.call<void>(
        "onResize",
        Nui::bind(
            [this](Nui::val obj, Nui::val)
            {
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

struct SingleTerminalChannel : public GenericTerminalChannel
{
    SingleChannelTerminalEngine* engine;

    void writeUser(std::string const& data) override
    {
        engine->write(data);
    }

    SingleTerminalChannel(
        SingleChannelTerminalEngine* engine,
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : GenericTerminalChannel{std::move(channelId), std::move(onLockedUserInput)}
        , engine{engine}
    {}
};

struct FrontendSessionManager::Implementation
{
    std::unique_ptr<TerminalEngine> engine;
    std::unordered_map<Ids::ChannelId, std::unique_ptr<TerminalChannel>, Ids::IdHash> channels;
    bool isMultiChannel;
    std::unique_ptr<SingleTerminalChannel> singleModeChannel;
    bool beingDisposed{false};
    bool disposeComplete{false};
    bool isInLockedMode{false};
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput;

    Implementation(
        std::unique_ptr<TerminalEngine> engine,
        bool isMultiChannel,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : engine{std::move(engine)}
        , channels{}
        , isMultiChannel{isMultiChannel}
        , singleModeChannel{}
        , onLockedUserInput{std::move(onLockedUserInput)}
    {}
};

#define CHECK_DISPOSAL() \
    if (isBeingDisposed()) \
    { \
        Log::warn("Cannot perform operation, frontend ssh manager is being disposed"); \
        return; \
    }

FrontendSessionManager::FrontendSessionManager(
    std::unique_ptr<TerminalEngine> engine,
    bool isMultiChannel,
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
)
    : impl_{std::make_unique<Implementation>(std::move(engine), isMultiChannel, std::move(onLockedUserInput))}
{}

void FrontendSessionManager::iterateAllChannels(
    std::function<bool(Ids::ChannelId const& channelId, TerminalChannel& channel)> const& handler
)
{
    CHECK_DISPOSAL()

    for (auto& [id, channel] : impl_->channels)
    {
        if (!handler(id, *channel))
            break;
    }
}

void FrontendSessionManager::connectionLossMode(bool isLocked)
{
    CHECK_DISPOSAL()

    impl_->isInLockedMode = isLocked;

    iterateAllChannels(
        [isLocked](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            channel.connectionLossMode(isLocked);
            return true;
        }
    );
}

void FrontendSessionManager::writeBroadcast(std::string const& msg)
{
    CHECK_DISPOSAL()

    iterateAllChannels(
        [&msg](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            channel.write(msg, false);
            return true;
        }
    );

    if (impl_->singleModeChannel)
    {
        impl_->singleModeChannel->doWrite(msg, false);
    }
}

void FrontendSessionManager::createChannel(
    Nui::val host,
    Persistence::TerminalOptions const& options,
    std::function<void(std::optional<Ids::ChannelId> /*channelId*/, std::string const& info)> onChannelCreated
)
{
    CHECK_DISPOSAL()

    using namespace std::string_literals;

    if (impl_->isInLockedMode)
    {
        Log::warn("Cannot create channel, frontend ssh manager is in locked mode");
        onChannelCreated(std::nullopt, "Cannot create channel, connection is lost");
        return;
    }

    Log::info("Creating channel");

    if (impl_->isMultiChannel)
    {
        auto* multiChannelEngine = static_cast<MultiChannelTerminalEngine*>(impl_->engine.get());

        std::shared_ptr<std::optional<Ids::ChannelId>> channelId =
            std::make_shared<std::optional<Ids::ChannelId>>(std::nullopt);

        Log::info("Creating channel using multi-channel engine");
        multiChannelEngine->createChannel(
            [this, channelId](std::string const& data)
            {
                // This should work, because the channel is opened after creation, and no data should be written before
                if (channelId && *channelId)
                {
                    if (auto channel = impl_->channels.find(**channelId); channel != impl_->channels.end())
                    {
                        channel->second->write(data, false);
                    }
                }
            },
            [this, channelId](std::string const& data)
            {
                // This should work, because the channel is opened after creation, and no data should be written before
                if (channelId && *channelId)
                {
                    if (auto channel = impl_->channels.find(**channelId); channel != impl_->channels.end())
                    {
                        channel->second->writeStderr(data, false);
                    }
                }
            },
            [this, channelId, onChannelCreated, host, options](std::optional<Ids::ChannelId> const& creationResult)
            {
                if (!creationResult)
                {
                    onChannelCreated(std::nullopt, "Failed to create channel");
                    return;
                }
                Log::info("Channel created.");

                *channelId = creationResult;

                auto* multiChannelEngine = static_cast<MultiChannelTerminalEngine*>(impl_->engine.get());

                if (!*channelId)
                {
                    onChannelCreated(std::nullopt, "Failed to create channel");
                    return;
                }

                [[maybe_unused]] auto [channelIter, _] = impl_->channels.emplace(
                    **channelId,
                    std::make_unique<TerminalChannel>(multiChannelEngine, **channelId, impl_->onLockedUserInput)
                );
                if (channelIter == impl_->channels.end())
                {
                    Log::error("Failed to create channel");
                    onChannelCreated(std::nullopt, "Failed to create channel");
                    return;
                }

                Log::info("Opening channel");
                channelIter->second->open(
                    host,
                    options,
                    [onChannelCreated, channelId = **channelId, host](bool success, std::string const& info) mutable
                    {
                        if (!success)
                        {
                            onChannelCreated(std::nullopt, info);
                            return;
                        }
                        host.call<void>("setAttribute", "data-channelid"s, channelId.value());
                        onChannelCreated(channelId, info);
                    }
                );
            }
        );
    }
    else
    {
        auto* singleChannelEngine = static_cast<SingleChannelTerminalEngine*>(impl_->engine.get());

        singleChannelEngine->open(
            [this, onChannelCreated = std::move(onChannelCreated), host, options](
                bool success, std::string const& infoOrUuid
            )
            {
                if (!success)
                {
                    Log::error("Failed to open terminal: '{}'", infoOrUuid);
                    onChannelCreated(std::nullopt, infoOrUuid);
                    return;
                };

                const auto channelId = Ids::makeChannelId(infoOrUuid);
                auto* singleChannelEngine = static_cast<SingleChannelTerminalEngine*>(impl_->engine.get());

                impl_->singleModeChannel =
                    std::make_unique<SingleTerminalChannel>(singleChannelEngine, channelId, impl_->onLockedUserInput);

                singleChannelEngine->setStderrHandler(
                    [this](std::string const& data)
                    {
                        impl_->singleModeChannel->doWrite(data, false);
                    }
                );
                singleChannelEngine->setStdoutHandler(
                    [this](std::string const& data)
                    {
                        // TODO: Add stderr styling mode
                        impl_->singleModeChannel->doWrite(data, false);
                    }
                );

                impl_->singleModeChannel->termId =
                    terminalUtility().call<std::string>("createTerminal", host, asVal(options));

                auto term = impl_->singleModeChannel->terminal();
                if (term.isUndefined())
                {
                    Log::error("Failed to get terminal with id: '{}", impl_->singleModeChannel->termId);
                    dispose([]() {});
                    onChannelCreated(std::nullopt, "Failed to get terminal");
                    return;
                }

                Log::info("Single channel terminal opened with id: '{}'", impl_->singleModeChannel->termId);

                term.call<void>(
                    "onData",
                    Nui::bind(
                        [this](Nui::val data, Nui::val)
                        {
                            impl_->singleModeChannel->doWrite(data.as<std::string>(), true);
                        },
                        std::placeholders::_1,
                        std::placeholders::_2
                    )
                );

                term.call<void>(
                    "onResize",
                    Nui::bind(
                        [this](Nui::val obj, Nui::val)
                        {
                            // Log::debug("FrontendSessionManager resized {}:{}. ", obj["cols"].as<int>(),
                            // obj["rows"].as<int>());
                            impl_->singleModeChannel->engine->resize(obj["cols"].as<int>(), obj["rows"].as<int>());
                        },
                        std::placeholders::_1,
                        std::placeholders::_2
                    )
                );

                if (!impl_->singleModeChannel->writeCache.empty())
                    impl_->singleModeChannel->doWrite("", false);
                onChannelCreated(channelId, "");
            }
        );
    }
}
TerminalChannel* FrontendSessionManager::channel(Ids::ChannelId const& channelId)
{
    if (isBeingDisposed())
    {
        Log::warn("Cannot get channel, frontend ssh manager is being disposed");
        return nullptr;
    }

    Log::debug("Getting channel: '{}'", channelId.value());
    if (auto channel = impl_->channels.find(channelId); channel != impl_->channels.end())
    {
        return channel->second.get();
    }
    return nullptr;
}
void FrontendSessionManager::closeChannel(Ids::ChannelId const& channelId)
{
    CHECK_DISPOSAL()

    if (auto channel = impl_->channels.find(channelId); channel != impl_->channels.end())
    {
        Log::info("Closing channel: '{}'", channelId.value());
        impl_->channels.erase(channel);
    }
}
void FrontendSessionManager::closeAllChannels()
{
    CHECK_DISPOSAL()

    Log::info("Closing all channels");
    impl_->channels.clear();
}

TerminalEngine& FrontendSessionManager::engine()
{
    return *impl_->engine;
}
void FrontendSessionManager::focus()
{
    CHECK_DISPOSAL()

    if (impl_->isMultiChannel)
    {
        if (!impl_->channels.empty())
        {
            impl_->channels.begin()->second->focus();
        }
    }
    else
    {
        if (!impl_->singleModeChannel->termId.empty())
        {
            impl_->singleModeChannel->terminal().call<void>("focus");
        }
    }
}

bool FrontendSessionManager::isBeingDisposed() const
{
    return impl_->beingDisposed;
}

void FrontendSessionManager::open(std::function<void(bool, std::string const&)> onOpen)
{
    CHECK_DISPOSAL()

    Log::info("Opening session");
    impl_->engine->open(
        [this, onOpen = std::move(onOpen)](bool success, std::string const& info)
        {
            if (!success)
            {
                Log::error("Failed to open terminal: '{}'", info);
                dispose(
                    [onOpen, info]()
                    {
                        onOpen(false, info);
                    }
                );

                return;
            };
            onOpen(true, info);
        }
    );
}
void FrontendSessionManager::dispose(std::function<void()> onComplete, bool recursion)
{
    if (impl_->disposeComplete)
    {
        onComplete();
        return;
    }

    if (!recursion && isBeingDisposed())
    {
        Log::warn("Frontend ssh manager, dispose already in progress");
        onComplete();
        return;
    }

    impl_->beingDisposed = true;

    /** Dont close backend channels, those will be cleaned up anyway by the entire session getting destroyed */
    for (auto& [id, channel] : impl_->channels)
    {
        Log::info("Disposing channel as apart of frontend ssh manager dispose: '{}'", id.value());
        channel->dispose(
            []()
            {
                Log::info("Disposed channel as apart of frontend ssh manager dispose");
            },
            false
        );
    }

    Log::info("No more channels to dispose, disposing engine.");
    impl_->engine->dispose(
        [this, onComplete = std::move(onComplete)]()
        {
            Log::info("Frontend ssh manager disposed.");
            impl_->disposeComplete = true;
            onComplete();
        }
    );
}
FrontendSessionManager::~FrontendSessionManager()
{
    dispose([]() {});
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(FrontendSessionManager);
