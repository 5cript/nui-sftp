#pragma once

#include <persistence/state_core.hpp>
#include <utility/describe.hpp>

#ifndef NUI_BACKEND
#    include <nui/frontend/val.hpp>
#endif

#include <string>
#include <vector>

namespace Persistence
{
    /**
     * @brief What the backend had to fix while loading the config file. The frontend maps each kind to a
     * translation, so the backend never produces user facing text.
     */
    BOOST_DEFINE_ENUM_CLASS(
        LoadWarningKind,
        ConfigUnreadable,
        UnknownEntriesRemoved,
        MissingFieldsDefaulted,
        AddedDefaultTermios,
        AddedDefaultTerminalOptions,
        AddedNebulaTerminalOptions,
        AddedDefaultSshOptions,
        AddedDefaultSftpOptions,
        AddedDefaultQueueOptions,
        AddedDefaultHistoryOptions,
        WroteMissingDefaults
    )

    /**
     * @brief A single config load warning.
     */
    struct LoadWarning
    {
        LoadWarningKind kind{LoadWarningKind::ConfigUnreadable};

        /**
         * @brief Untranslated values for the placeholders of the translation, such as a file path, a json diff or
         * an exception text.
         */
        std::vector<std::string> arguments{};
    };
    BOOST_DESCRIBE_STRUCT(LoadWarning, (), (kind, arguments))

    using LoadWarnings = std::vector<LoadWarning>;

#ifndef NUI_BACKEND
    /**
     * @brief Reads the warnings the backend sent as a json array. Logs and returns what was read so far if the
     * value does not match.
     *
     * @param warnings The value of the "warnings" property of a backend response.
     */
    LoadWarnings parseLoadWarnings(Nui::val const& warnings);
#endif
}
