#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace FileTransferProtocol::Test
{
    struct ServerUser
    {
        std::string name{};
        std::string password{};
    };

    /**
     * @brief One container of a server: default, throttled for faults in mid-transfer, limits for connection limits.
     */
    struct ServerVariant
    {
        /** @brief Given to the entry point of the image. */
        std::vector<std::string> command{};
        std::vector<std::string> runArguments{};
    };

    /**
     * @brief What works on a server, which is not always what its FEAT says.
     */
    struct ServerFeatures
    {
        bool mlsd{false};
        bool mlst{false};
        bool mfmt{false};
        bool utf8{false};
        bool epsv{false};
    };

    /**
     * @brief The same behaviour can differ between plain connections and TLS.
     */
    template <typename Value>
    struct PerSecurity
    {
        Value plain{};
        Value tls{};
    };

    enum class ModificationTimeCommand
    {
        Mfmt,
        Mdtm,
        SiteUtime,
    };

    enum class MissingReuse
    {
        EndsSession,
        KeepsSession,
    };

    enum class ConnectionLimit
    {
        AtGreeting,
        AfterPass,
    };

    enum class ListOfMissingDirectory
    {
        EmptyListing,
        FailsAfterPreliminary,
    };

    /**
     * @brief How a server behaves where servers disagree, the mechanism behind an outcome that must not differ.
     */
    struct ServerExpectations
    {
        ServerFeatures features{};
        /** @brief What its FEAT reply lists at least. */
        std::vector<std::string> advertised{};
        ModificationTimeCommand modificationTime{};
        /** @brief The replies to an ABOR after the data connection closed, in order. */
        PerSecurity<std::vector<int>> abortReplies{};
        /** @brief Whether the final reply of a download waits until the client closed the data connection. */
        PerSecurity<bool> finalReplyWaitsForDataClose{};
        bool sessionReuseRequired{false};
        MissingReuse missingReuse{};
        ConnectionLimit connectionLimit{};
        /** @brief Whether the text of a 550 says what went wrong. */
        bool repliesTell550Causes{false};
        ListOfMissingDirectory listOfMissingDirectory{};
    };

    /**
     * @brief A server of the conformance suites: everything in its directory below servers/.
     */
    struct ServerProfile
    {
        /** @brief The name of its directory. */
        std::string name{};
        std::filesystem::path directory{};
        std::string image{};
        std::vector<std::string> runArguments{};
        std::uint16_t plainPort{0u};
        std::uint16_t explicitPort{0u};
        std::uint16_t implicitPort{0u};
        std::vector<ServerUser> users{};
        std::map<std::string, ServerVariant> variants{};
        ServerExpectations expectations{};
    };

    /**
     * @brief Reads a profile.json.
     *
     * @return The profile, or what is wrong with the file.
     */
    std::expected<ServerProfile, std::string> loadServerProfile(std::filesystem::path const& file);
}
