#include <ftp/support/server_profile.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using nlohmann::json;

        template <typename Enum>
        using EnumNames = std::initializer_list<std::pair<std::string_view, Enum>>;

        /**
         * @brief Reads the enum in @p object at @p key, adding to @p problems when it is no known name.
         */
        template <typename Enum>
        Enum readEnum(json const& object, char const* key, EnumNames<Enum> names, std::string& problems)
        {
            const auto text = object.at(key).get<std::string>();
            const auto found = std::ranges::find_if(
                names,
                [&text](auto const& name)
                {
                    return name.first == text;
                }
            );
            if (found == names.end())
            {
                problems += fmt::format("{}: unknown value '{}'. ", key, text);
                return Enum{};
            }
            return found->second;
        }

        ServerExpectations readExpectations(json const& object, std::string& problems)
        {
            auto const& features = object.at("features");
            return ServerExpectations{
                .features =
                    ServerFeatures{
                        .mlsd = features.at("mlsd").get<bool>(),
                        .mlst = features.at("mlst").get<bool>(),
                        .mfmt = features.at("mfmt").get<bool>(),
                        .utf8 = features.at("utf8").get<bool>(),
                        .epsv = features.at("epsv").get<bool>(),
                    },
                .advertised = object.at("advertised").get<std::vector<std::string>>(),
                .modificationTime = readEnum<ModificationTimeCommand>(
                    object,
                    "modificationTime",
                    {{"mfmt", ModificationTimeCommand::Mfmt},
                     {"mdtm", ModificationTimeCommand::Mdtm},
                     {"siteUtime", ModificationTimeCommand::SiteUtime}},
                    problems
                ),
                .repliesTell550Causes = object.at("repliesTell550Causes").get<bool>(),
                .resumesDataSessions = object.at("resumesDataSessions").get<bool>(),
            };
        }

        ServerProfile readProfile(json const& object, std::filesystem::path const& file, std::string& problems)
        {
            ServerProfile profile{
                .name = file.parent_path().filename().generic_string(),
                .directory = file.parent_path(),
                .image = object.at("image").get<std::string>(),
                .runArguments = object.at("runArguments").get<std::vector<std::string>>(),
                .plainPort = object.at("ports").at("plain").get<std::uint16_t>(),
                .explicitPort = object.at("ports").at("explicit").get<std::uint16_t>(),
                .implicitPort = object.at("ports").at("implicit").get<std::uint16_t>(),
                .expectations = readExpectations(object.at("expectations"), problems),
            };
            for (auto const& user : object.at("users"))
            {
                profile.users.push_back({
                    .name = user.at("name").get<std::string>(),
                    .password = user.at("password").get<std::string>(),
                });
            }
            for (auto const& [name, variant] : object.at("variants").items())
            {
                profile.variants[name] = ServerVariant{
                    .command = variant.at("command").get<std::vector<std::string>>(),
                    .runArguments = variant.value("runArguments", std::vector<std::string>{}),
                };
            }
            return profile;
        }
    }

    std::expected<ServerProfile, std::string> loadServerProfile(std::filesystem::path const& file)
    {
        std::ifstream stream{file};
        if (!stream)
            return std::unexpected(fmt::format("Cannot read {}", file.generic_string()));

        try
        {
            std::string problems{};
            auto profile = readProfile(json::parse(stream), file, problems);
            if (!problems.empty())
                return std::unexpected(fmt::format("{}: {}", file.generic_string(), problems));
            return profile;
        }
        catch (json::exception const& error)
        {
            return std::unexpected(fmt::format("{}: {}", file.generic_string(), error.what()));
        }
    }
}
