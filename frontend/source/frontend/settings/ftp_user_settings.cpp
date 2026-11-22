#include <frontend/settings/ftp_user_settings.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements/fragment.hpp>

#include <fmt/format.h>

FtpUserSettings::FtpUserSettings(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    Nui::Observed<bool>* hasEntry
)
    : name{factory.identity({"ftpSessionOptions", "user"}), onChange, nulloptReset(name, onChange), hasEntry}
    , account{factory.identity({"ftpSessionOptions", "account"}), onChange, nulloptReset(account, onChange), hasEntry}
{}

void FtpUserSettings::applyToState(StateType& state) const
{
    assignIfValid(state.name, name);
    assignIfValid(state.account, account);
}

void FtpUserSettings::loadFromState(StateType const& state)
{
    name.value(state.name);
    account.value(state.account);
}

Nui::ElementRenderer FtpUserSettings::render()
{
    return Nui::Elements::fragment(name(), account());
}

std::string ftpUserLabel(Persistence::FtpUser const& user)
{
    const auto userName =
        user.name && !user.name->empty() ? *user.name : language->get("settings", "ftpSessionOptions", "anonymousUser");
    if (!user.account || user.account->empty())
        return userName;
    return fmt::format("{} ({})", userName, *user.account);
}
