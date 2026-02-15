#include <frontend/settings/session_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/session_icon_options.hpp>

#include <log/log.hpp>

#include <utility/enum_string_convert.hpp>

#include <svgs/laptop.hpp>
#include <svgs/ipad.hpp>
#include <svgs/iphone.hpp>
#include <svgs/account.hpp>
#include <svgs/accessibility.hpp>
#include <svgs/area-chart.hpp>
#include <svgs/favorite.hpp>
#include <svgs/fax-machine.hpp>
#include <svgs/flag.hpp>
#include <svgs/family-care.hpp>
#include <svgs/home.hpp>
#include <svgs/home-share.hpp>
#include <svgs/heart.hpp>
#include <svgs/heart-2.hpp>
#include <svgs/key.hpp>
#include <svgs/feed.hpp>
#include <svgs/it-instance.hpp>
#include <svgs/it-host.hpp>
#include <svgs/it-system.hpp>
#include <svgs/lab.hpp>
#include <svgs/machine.hpp>
#include <svgs/meal.hpp>
#include <svgs/physical-activity.hpp>
#include <svgs/primary-key.hpp>
#include <svgs/shipping-status.hpp>
#include <svgs/shield.hpp>
#include <svgs/study-leave.hpp>
#include <svgs/subway-train.hpp>
#include <svgs/syringe.hpp>
#include <svgs/tag.hpp>
#include <svgs/web-cam.hpp>
#include <svgs/sound-loud.hpp>
#include <svgs/simple-payment.hpp>
#include <svgs/print.hpp>
#include <svgs/nutrition-activity.hpp>
#include <svgs/lightbulb.hpp>

using namespace std::string_literals;

SessionOptions::SessionOptions(std::function<void()> const& onChange, std::function<std::optional<nlohmann::json>()> const& obtainCurrentLayout, ConfirmDialog* confirmDialog, InputDialog* newItemDialog)
    : terminalEngineType{
          {
              Persistence::TerminalEngineType::shell,
              Persistence::TerminalEngineType::ssh,
          },
          language->getObserved("settings", "sessionOptions", "terminalEngineTypeHelpText"),
          onChange,
          valueReset(
              terminalEngineType,
              onChange,
              Persistence::SessionOptions{}.type
          ),
          [](Persistence::TerminalEngineType const& v)
          {
              return Utility::enumToString(v);
        }
      }
    , icon{
        [](){
            std::vector<std::string> icons;
            for (const auto iconName : sessionIconOptions)
            {
                icons.push_back(std::string{iconName});
            }
            return icons;
        }(),
          language->getObserved("settings", "sessionOptions", "iconHelpText"),
          onChange,
          valueReset(icon, onChange, Persistence::SessionOptions{}.icon),
            [](std::string const& icon)
            {
                return icon;
            },
            [](std::string const& icon) -> Nui::ElementRenderer
            {
                using namespace GeneratedSvgs;

                if (icon == "laptop")
                    return laptop();
                if (icon == "ipad")
                    return ipad();
                if (icon == "iphone")
                    return iphone();
                if (icon == "account")
                    return account();
                if (icon == "accessibility")
                    return accessibility();
                if (icon == "area-chart")
                    return areachart();
                if (icon == "favorite")
                    return favorite();
                if (icon == "fax-machine")
                    return faxmachine();
                if (icon == "flag")
                    return flag();
                if (icon == "family-care")
                    return familycare();
                if (icon == "home")
                    return home();
                if (icon == "home-share")
                    return homeshare();
                if (icon == "heart")
                    return heart();
                if (icon == "heart-2")
                    return heart2();
                if (icon == "key")
                    return key();
                if (icon == "feed")
                    return feed();
                if (icon == "it-instance")
                    return itinstance();
                if (icon == "it-host")
                    return ithost();
                if (icon == "it-system")
                    return itsystem();
                if (icon == "lab")
                    return lab();
                if (icon == "machine")
                    return machine();
                if (icon == "meal")
                    return meal();
                if (icon == "physical-activity")
                    return physicalactivity();
                if (icon == "primary-key")
                    return primarykey();
                if (icon == "shipping-status")
                    return shippingstatus();
                if (icon == "shield")
                    return shield();
                if (icon == "study-leave")
                    return studyleave();
                if (icon == "subway-train")
                    return subwaytrain();
                if (icon == "syringe")
                    return syringe();
                if (icon == "tag")
                    return tag();
                if (icon == "web-cam")
                    return webcam();
                if (icon == "sound-loud")
                    return soundloud();
                if (icon == "simple-payment")
                    return simplepayment();
                if (icon == "print")
                    return print();
                if (icon == "nutrition-activity")
                    return nutritionactivity();
                if (icon == "lightbulb")
                    return lightbulb();

                return Nui::nil();
            },
      }
    , orderBy{
          language->getObserved("settings", "sessionOptions", "orderByHelpText"),
          onChange,
          nulloptReset(orderBy, onChange)
      }
    , isStartupSession{
          language->getObserved("settings", "sessionOptions", "isStartupSessionHelpText"),
          onChange,
          valueReset(isStartupSession, onChange, Persistence::SessionOptions{}.startupSession)
      }
    , layout{
          language->getObserved("settings", "sessionOptions", "layoutHelpText"),
          onChange,
          obtainCurrentLayout,
          confirmDialog,
          newItemDialog
      }
    , terminalOptions{onChange}
    , termios{onChange}
    , queueOptions{onChange}
    , executingSessionOptions{onChange}
    , sshSessionOptions{onChange}
{}

void SessionOptions::applyToState(Persistence::SessionOptions& state) const
{
    state.type = terminalEngineType.value();
    state.icon = icon.value();
    state.orderBy = orderBy.value();
    state.startupSession = isStartupSession.value();
    state.layouts = layout.value();
    terminalOptions.applyToState(state.terminalOptions.value());
    termios.applyToState(state.termios.value());
    queueOptions.applyToState(state.queueOptions.value());

    if (std::holds_alternative<Persistence::SshSessionOptions>(state.engine))
    {
        sshSessionOptions.applyToState(state.engine.emplace<Persistence::SshSessionOptions>());
    }
    else if (std::holds_alternative<Persistence::ExecutingSessionOptions>(state.engine))
    {
        executingSessionOptions.applyToState(state.engine.emplace<Persistence::ExecutingSessionOptions>());
    }

    Log::debug("SessionOptions::applyToState: setting references:");
    Log::debug(
        "TerminalOptions group key: {}", terminalOptions.groupKey->has_value() ? **terminalOptions.groupKey : "nullopt"
    );
    state.terminalOptions.ref(*terminalOptions.groupKey);
    Log::debug("Termios group key: {}", termios.groupKey->has_value() ? **termios.groupKey : "nullopt");
    state.termios.ref(*termios.groupKey);
    Log::debug("QueueOptions group key: {}", queueOptions.groupKey->has_value() ? **queueOptions.groupKey : "nullopt");
    state.queueOptions.ref(*queueOptions.groupKey);
}

void SessionOptions::loadFromState(Persistence::SessionOptions const& state, bool loadRefs)
{
    terminalEngineType.value(state.type);
    icon.value(state.icon);
    orderBy.value(state.orderBy);
    isStartupSession.value(state.startupSession);
    layout.value(state.layouts);
    terminalOptions.loadFromState(state.terminalOptions.value());
    termios.loadFromState(state.termios.value());
    queueOptions.loadFromState(state.queueOptions.value());

    if (std::holds_alternative<Persistence::SshSessionOptions>(state.engine))
    {
        sshSessionOptions.loadFromState(std::get<Persistence::SshSessionOptions>(state.engine), loadRefs);
    }
    else if (std::holds_alternative<Persistence::ExecutingSessionOptions>(state.engine))
    {
        executingSessionOptions.loadFromState(std::get<Persistence::ExecutingSessionOptions>(state.engine), loadRefs);
    }
    else
    {
        Log::warn("SessionOptions::loadFromState: engine variant holds no value.");
    }

    if (loadRefs)
    {
        terminalOptions.groupKey = state.terminalOptions.hasReference()
            ? std::optional<std::string>{state.terminalOptions.ref()}
            : std::nullopt;
        termios.groupKey =
            state.termios.hasReference() ? std::optional<std::string>{state.termios.ref()} : std::nullopt;
        queueOptions.groupKey =
            state.queueOptions.hasReference() ? std::optional<std::string>{state.queueOptions.ref()} : std::nullopt;
    }
}

void SessionOptions::assumeDefaultsFrom(Persistence::SessionOptions const& state)
{
    terminalOptions.assumeDefaultsFrom(state.terminalOptions.value());
    termios.assumeDefaultsFrom(state.termios.value());
    queueOptions.assumeDefaultsFrom(state.queueOptions.value());
    if (std::holds_alternative<Persistence::ExecutingSessionOptions>(state.engine))
    {
        executingSessionOptions.assumeDefaultsFrom(std::get<Persistence::ExecutingSessionOptions>(state.engine));
    }
    else if (std::holds_alternative<Persistence::SshSessionOptions>(state.engine))
    {
        sshSessionOptions.assumeDefaultsFrom(std::get<Persistence::SshSessionOptions>(state.engine));
    }
}