#include <persistence/legacy_migration.hpp>

#include <array>
#include <string_view>

namespace Persistence
{
    namespace
    {
        /**
         * @brief Copies the flat user and key fields of an SSH engine into a single entry of its user list. The flat
         * fields stay for earlier versions.
         */
        bool migrateSshUser(nlohmann::json& sshEngine)
        {
            if (!sshEngine.is_object() || sshEngine.contains("users"))
                return false;

            constexpr std::array<std::pair<std::string_view, std::string_view>, 3> legacyFields{{
                {"user", "name"},
                {"sshKeyPrivate", "sshKeyPrivate"},
                {"sshKeyPublic", "sshKeyPublic"},
            }};

            auto user = nlohmann::json::object();
            for (auto const& [legacyName, currentName] : legacyFields)
            {
                if (const auto iter = sshEngine.find(legacyName); iter != sshEngine.end())
                    user[std::string{currentName}] = *iter;
            }

            sshEngine["users"] = user.empty() ? nlohmann::json::array() : nlohmann::json::array({std::move(user)});
            return true;
        }
    }

    bool migrateLegacyEntries(nlohmann::json& config)
    {
        if (!config.is_object())
            return false;

        bool migrated = false;
        if (const auto sessions = config.find("sessions"); sessions != config.end() && sessions->is_object())
        {
            for (auto& session : *sessions)
            {
                if (!session.is_object() || session.value("type", std::string{}) != "ssh")
                    continue;
                if (const auto engine = session.find("engine"); engine != session.end())
                    migrated = migrateSshUser(*engine) || migrated;
            }
        }

        if (const auto lastDirectConnect = config.find("lastDirectConnect"); lastDirectConnect != config.end())
            migrated = migrateSshUser(*lastDirectConnect) || migrated;

        return migrated;
    }
}
