#include <frontend/settings/ssh_user_settings.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements/fragment.hpp>

#include <fmt/format.h>

SshUserSettings::SshUserSettings(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    Nui::Observed<bool>* hasEntry
)
    : name{factory.identity({"sessionOptions", "user"}), onChange, nulloptReset(name, onChange), hasEntry}
    , sshKeyPrivate{
          factory.identity({"sessionOptions", "sshKeyPrivate"}),
          PathSettingType::File,
          onChange,
          nulloptReset(sshKeyPrivate, onChange),
          hasEntry,
      }
    , sshKeyPublic{
          factory.identity({"sessionOptions", "sshKeyPublic"}),
          PathSettingType::File,
          onChange,
          nulloptReset(sshKeyPublic, onChange),
          hasEntry,
      }
{}

void SshUserSettings::applyToState(StateType& state) const
{
    assignIfValid(state.name, name);
    assignIfValid(state.sshKeyPrivate, sshKeyPrivate);
    assignIfValid(state.sshKeyPublic, sshKeyPublic);
}

void SshUserSettings::loadFromState(StateType const& state)
{
    name.value(state.name);
    sshKeyPrivate.value(state.sshKeyPrivate);
    sshKeyPublic.value(state.sshKeyPublic);
}

Nui::ElementRenderer SshUserSettings::render()
{
    return Nui::Elements::fragment(name(), sshKeyPrivate(), sshKeyPublic());
}

std::string sshUserLabel(Persistence::SshUser const& user)
{
    const auto userName = user.name.value_or(language->get("settings", "sessionOptions", "userWithoutName"));
    if (!user.sshKeyPrivate || user.sshKeyPrivate->empty())
        return userName;
    return fmt::format("{} ({})", userName, user.sshKeyPrivate->filename().string());
}
