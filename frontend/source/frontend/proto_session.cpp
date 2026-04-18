#include <frontend/proto_session.hpp>

#include <frontend/terminal/executing_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <log/log.hpp>

#include <utility>
#include <variant>

struct ProtoSession::Implementation
{
    Persistence::SessionOptions sessionOptions;
    Persistence::UiOptions uiOptions;
    std::optional<SessionSnapshot> resumeFromSnapshot;
    std::function<void(ProtoSession*)> onReady;
    std::function<void(ProtoSession*, std::string const&)> onFailed;

    std::unique_ptr<FrontendSessionManager> frontendSessionManager;
    std::string engineNameCache;
    bool started{false};

    explicit Implementation(ProtoSession::Params&& params)
        : sessionOptions{std::move(params.sessionOptions)}
        , uiOptions{std::move(params.uiOptions)}
        , resumeFromSnapshot{std::move(params.resumeFromSnapshot)}
        , onReady{std::move(params.onReady)}
        , onFailed{std::move(params.onFailed)}
    {}
};

ProtoSession::ProtoSession(Params params)
    : impl_{std::make_unique<Implementation>(std::move(params))}
{
    // Build the engine now so engineName() is valid even before start() runs.
    // Locked-mode input has no session-level UI to route to during the probe
    // phase, so we wire an explicit no-op — Session::Session(ProtoSession,…)
    // rebinds this on adoption via FrontendSessionManager::setLockedUserInputHandler.
    auto noopLockedInput = [](Ids::ChannelId, std::string const&) {};

    if (std::holds_alternative<Persistence::SshSessionOptions>(impl_->sessionOptions.engine))
    {
        // onConnectionLoss is left unbound here — if the transport drops
        // between open() and adoption the probe fails cleanly via onOpen,
        // and after adoption Session::Session rebinds this to route into
        // the lost-connection overlay.
        impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
            std::make_unique<SshTerminalEngine>(SshTerminalEngine::Settings{
                .sessionOptions = impl_->sessionOptions,
                .onConnectionLoss = {},
            }),
            std::move(noopLockedInput),
            /*eagerAuxEngine=*/true
        );
        impl_->engineNameCache = "ssh";
    }
    else
    {
        impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
            std::make_unique<ExecutingTerminalEngine>(ExecutingTerminalEngine::Settings{
                .onProcessChange = {},
            }),
            std::move(noopLockedInput)
        );
        impl_->engineNameCache = "local";
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(ProtoSession);

void ProtoSession::start()
{
    if (impl_->started)
    {
        Log::warn("ProtoSession::start called twice — ignored");
        return;
    }
    impl_->started = true;

    impl_->frontendSessionManager->open(
        [this](bool success, std::string const& info) {
            if (success)
            {
                Log::info("ProtoSession: transport open succeeded");
                if (impl_->onReady)
                    impl_->onReady(this);
                return;
            }
            Log::warn("ProtoSession: transport open failed: {}", info);
            if (impl_->onFailed)
                impl_->onFailed(this, info);
        }
    );
}

Persistence::SessionOptions ProtoSession::takeSessionOptions()
{
    return std::move(impl_->sessionOptions);
}

Persistence::UiOptions ProtoSession::takeUiOptions()
{
    return std::move(impl_->uiOptions);
}

std::unique_ptr<FrontendSessionManager> ProtoSession::takeFrontendSessionManager()
{
    return std::move(impl_->frontendSessionManager);
}

std::optional<SessionSnapshot> ProtoSession::takeResumeSnapshot()
{
    auto snap = std::move(impl_->resumeFromSnapshot);
    impl_->resumeFromSnapshot.reset();
    return snap;
}

std::string ProtoSession::engineName() const
{
    return impl_->engineNameCache;
}
