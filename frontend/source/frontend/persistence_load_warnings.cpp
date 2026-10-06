#include <frontend/persistence_load_warnings.hpp>
#include <utility/language.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <vector>

namespace
{
    std::string translation(Persistence::LoadWarningKind kind)
    {
        using enum Persistence::LoadWarningKind;
        switch (kind)
        {
            case ConfigUnreadable:
                return language->get("persistence", "loadWarnings", "configUnreadable");
            case UnknownEntriesRemoved:
                return language->get("persistence", "loadWarnings", "unknownEntriesRemoved");
            case MissingFieldsDefaulted:
                return language->get("persistence", "loadWarnings", "missingFieldsDefaulted");
            case AddedDefaultTermios:
                return language->get("persistence", "loadWarnings", "addedDefaultTermios");
            case AddedDefaultTerminalOptions:
                return language->get("persistence", "loadWarnings", "addedDefaultTerminalOptions");
            case AddedNebulaTerminalOptions:
                return language->get("persistence", "loadWarnings", "addedNebulaTerminalOptions");
            case AddedDefaultSshOptions:
                return language->get("persistence", "loadWarnings", "addedDefaultSshOptions");
            case AddedDefaultSftpOptions:
                return language->get("persistence", "loadWarnings", "addedDefaultSftpOptions");
            case AddedDefaultQueueOptions:
                return language->get("persistence", "loadWarnings", "addedDefaultQueueOptions");
            case AddedDefaultHistoryOptions:
                return language->get("persistence", "loadWarnings", "addedDefaultHistoryOptions");
            case WroteMissingDefaults:
                return language->get("persistence", "loadWarnings", "wroteMissingDefaults");
        }
        return {};
    }
}

std::string formatLoadWarnings(Persistence::LoadWarnings const& warnings)
{
    std::vector<std::string> paragraphs{};
    paragraphs.reserve(warnings.size());
    for (auto const& warning : warnings)
        paragraphs.push_back(formatWithArguments(translation(warning.kind), warning.arguments));
    return fmt::format("{}", fmt::join(paragraphs, "\n"));
}
